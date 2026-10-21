#include "render/gpu.hpp"

#include "core/work_counter.hpp"
#include "render/draw_list.hpp"
#include "render/raster.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unordered_map>
#include <vector>

namespace mf {
namespace {

id<MTLDevice> metalDevice() {
    static id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return device;
}

// CPU images own complete VM regions. Attach a Metal view only when needed and
// reuse it with the region, avoiding GPU setup for CPU-only image allocations.
class PixelStorage {
    struct Block {
        void* data = nullptr;
        size_t bytes = 0;
        id<MTLBuffer> buffer = nil;
    };
    static constexpr size_t kBudget = 64 * 1024 * 1024;
    std::array<Block, 32> free_{};
    std::unordered_map<void*, Block> live_;
    size_t count_ = 0, bytes_ = 0;
    std::mutex mutex_;

    static void release(Block& b) {
        if (b.data && !b.buffer) munmap(b.data, b.bytes);
        b = {};
    }
    void remove(size_t i) {
        bytes_ -= free_[i].bytes;
        std::move(free_.begin() + i + 1, free_.begin() + count_, free_.begin() + i);
        free_[--count_] = {};
    }

public:
    ~PixelStorage() {
        for (size_t i = 0; i < count_; ++i) release(free_[i]);
    }

    void* allocate(size_t bytes) {
        if (!bytes) return nullptr;
        bytes = detail::pixelAllocationSize(bytes);
        Block block;
        {
            std::lock_guard lock(mutex_);
            for (size_t i = count_; i > 0; --i) {
                if (free_[i - 1].bytes != bytes) continue;
                block = free_[i - 1];
                remove(i - 1);
                break;
            }
        }
        if (!block.data) {
            void* data = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (data == MAP_FAILED) throw std::bad_alloc();
            block = {data, bytes};
        }
        try {
            std::lock_guard lock(mutex_);
            live_.emplace(block.data, block);
        } catch (...) {
            release(block);
            throw;
        }
        return block.data;
    }

    void free(void* data) noexcept {
        if (!data) return;
        std::lock_guard lock(mutex_);
        auto it = live_.find(data);
        Block block = it->second;
        live_.erase(it);
        if (block.bytes > kBudget) {
            release(block);
            return;
        }
        while (count_ && (count_ == free_.size() || bytes_ + block.bytes > kBudget)) {
            Block old = free_[0];
            remove(0);
            release(old);
        }
        free_[count_++] = block;
        bytes_ += block.bytes;
    }

    id<MTLBuffer> buffer(void* data) {
        std::lock_guard lock(mutex_);
        auto it = live_.find(data);
        if (it == live_.end()) return nil;
        Block& block = it->second;
        if (@available(macOS 10.15, *)) {
            id<MTLDevice> device = metalDevice();
            if (!block.buffer && device.hasUnifiedMemory && block.bytes <= device.maxBufferLength) {
                // Transfer the VM region's final release to the view. Metal
                // may retain it beyond the last CPU image or autorelease pool.
                __block bool ownsMemory = false;
                id<MTLBuffer> view = [device newBufferWithBytesNoCopy:block.data
                                                              length:block.bytes
                                                             options:MTLResourceStorageModeShared
                                                         deallocator:^(void* data, NSUInteger length) {
                                                             if (ownsMemory) munmap(data, length);
                                                         }];
                if (view) {
                    ownsMemory = true;
                    block.buffer = view;
                }
            }
        }
        return block.buffer;
    }
};

PixelStorage& pixelStorage() {
    static PixelStorage storage;
    return storage;
}

// Scratch and statistics buffers have no CPU image owner. A frame leases them
// until its commands complete, then returns them to this bounded shared pool.
class TemporaryBuffers {
    static constexpr size_t kBudget = 64 * 1024 * 1024;
    std::array<id<MTLBuffer>, 32> free_{};
    size_t count_ = 0, bytes_ = 0;
    std::mutex mutex_;

    void remove(size_t index) {
        bytes_ -= free_[index].length;
        std::move(free_.begin() + index + 1, free_.begin() + count_, free_.begin() + index);
        free_[--count_] = nil;
    }

public:
    id<MTLBuffer> acquire(id<MTLDevice> device, size_t bytes) {
        {
            std::lock_guard lock(mutex_);
            for (size_t i = count_; i > 0; --i) {
                if (free_[i - 1].length != bytes) continue;
                id<MTLBuffer> result = free_[i - 1];
                remove(i - 1);
                return result;
            }
        }
        return [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    }

    void recycle(id<MTLBuffer> buffer) {
        size_t bytes = buffer.length;
        if (!buffer || bytes > kBudget) return;
        std::lock_guard lock(mutex_);
        while (count_ && (count_ == free_.size() || bytes_ + bytes > kBudget)) remove(0);
        free_[count_++] = buffer;
        bytes_ += bytes;
    }
};

TemporaryBuffers& temporaryBuffers() {
    static TemporaryBuffers buffers;
    return buffers;
}

constexpr const char* kShaders = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct DrawParams {
    uint4 sizes;
    int4 bounds;
    float4 matrix;
    float4 offset;
    float4 props;
    float4 color;
};
struct TransitionParams {
    uint4 sizes;
    int4 shifts;
    float4 props;
    float4 color;
};
struct EffectParams {
    uint4 sizes;
    float4 p0;
    float4 p1;
    float4 color;
};

uchar4 pack(float4 c) { return uchar4(clamp(c * 255.0f + 0.5f, 0.0f, 255.0f)); }
float4 unpack(uchar4 c) { return float4(c) / 255.0f; }

float4 blend(float4 s, float4 d, uint mode) {
    switch (mode) {
        case 1: return min(1.0f, s + d);
        case 2: return float4(s.rgb * d.rgb + s.rgb * (1.0f - d.a) + d.rgb * (1.0f - s.a),
                              s.a + d.a * (1.0f - s.a));
        case 3: return float4(s.rgb + d.rgb - s.rgb * d.rgb, s.a + d.a * (1.0f - s.a));
        default: return s + d * (1.0f - s.a);
    }
}

// Each threadgroup owns its result slot. Reduce locally instead of making the
// entire GPU contend on three global atomic counters.
void countWork(device packed_uint3* counts, uint3 work, uint lane, uint simd, uint simds,
               threadgroup uint3* partial, uint2 group, uint2 groups) {
    uint3 subtotal = simd_sum(work);
    if (lane == 0) partial[simd] = subtotal;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (simd == 0) {
        uint3 total = simd_sum(lane < simds ? partial[lane] : uint3(0));
        if (lane == 0) counts[group.y * groups.x + group.x] = packed_uint3(total);
    }
}

kernel void fill_image(device uchar4* out [[buffer(0)]], constant uint4& sizes [[buffer(1)]],
                       constant uchar4& color [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {
    out[xy.y * sizes.x + xy.x] = color;
}

// An aligned solid covers every pixel in its clipped bounds. Its work counts
// are known on the CPU, so no threadgroup reduction or counter buffer is needed.
kernel void solid_region(device uchar4* dst [[buffer(0)]], constant DrawParams& p [[buffer(1)]],
                         uint2 xy [[thread_position_in_grid]]) {
    uint index = (p.bounds.y + xy.y) * p.sizes.x + p.bounds.x + xy.x;
    uchar4 raw = pack(p.color);
    if (raw.a == 255 && p.props.x == 1 && p.props.y == 0) dst[index] = raw;
    else dst[index] = pack(blend(unpack(raw) * p.props.x, unpack(dst[index]), uint(p.props.y)));
}

kernel void draw_layer(device const uchar4* src [[buffer(0)]], device uchar4* dst [[buffer(1)]],
                       constant DrawParams& p [[buffer(2)]], device packed_uint3* counts [[buffer(3)]],
                       uint2 xy [[thread_position_in_grid]], uint lane [[thread_index_in_simdgroup]],
                       uint simd [[simdgroup_index_in_threadgroup]], uint simds [[simdgroups_per_threadgroup]],
                       uint2 group [[threadgroup_position_in_grid]], uint2 groups [[threadgroups_per_grid]]) {
    threadgroup uint3 partial[8];
    int x = p.bounds.x + int(xy.x), y = p.bounds.y + int(xy.y);
    uint taps = 0, blends = 0, copies = 0;
    float4 s = 0;
    uchar4 raw = 0;
    bool aligned = p.props.z != 0, solid = p.props.w != 0;
    if (aligned) {
        int sx = x + int(p.offset.x), sy = y + int(p.offset.y);
        if (sx >= 0 && sy >= 0 && sx < int(p.sizes.z) && sy < int(p.sizes.w)) {
            raw = solid ? pack(p.color) : src[sy * p.sizes.z + sx];
            s = unpack(raw);
        }
    } else {
        float2 pos = float2(x, y) + 0.5f;
        float u = dot(pos, p.matrix.xy) + p.offset.x;
        float v = dot(pos, p.matrix.zw) + p.offset.y;
        if (u > -1 && v > -1 && u < p.sizes.z && v < p.sizes.w) {
            int2 base = int2(floor(float2(u, v)));
            float2 f = float2(u, v) - float2(base);
            float4 samples[4];
            for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                int sx = base.x + dx, sy = base.y + dy;
                bool inside = sx >= 0 && sy >= 0 && sx < int(p.sizes.z) && sy < int(p.sizes.w);
                samples[dy * 2 + dx] = inside ? (solid ? p.color : unpack(src[sy * p.sizes.z + sx])) : float4(0);
                taps += inside && !solid;
            }
            s = mix(mix(samples[0], samples[1], f.x), mix(samples[2], samples[3], f.x), f.y);
        }
    }
    if (s.a > 0) {
        uint index = y * p.sizes.x + x;
        if (aligned && raw.a == 255 && p.props.x == 1 && p.props.y == 0) {
            dst[index] = raw;
            copies = 1;
        } else {
            dst[index] = pack(blend(s * p.props.x, unpack(dst[index]), uint(p.props.y)));
            blends = 1;
        }
    }
    countWork(counts, uint3(blends, taps, copies), lane, simd, simds, partial, group, groups);
}

kernel void transition_image(device const uchar4* a [[buffer(0)]], device const uchar4* b [[buffer(1)]],
                             device uchar4* out [[buffer(2)]], constant TransitionParams& p [[buffer(3)]],
                             device packed_uint3* counts [[buffer(4)]], uint2 xy [[thread_position_in_grid]],
                             uint lane [[thread_index_in_simdgroup]], uint simd [[simdgroup_index_in_threadgroup]],
                             uint simds [[simdgroups_per_threadgroup]], uint2 group [[threadgroup_position_in_grid]],
                             uint2 groups [[threadgroups_per_grid]]) {
    threadgroup uint3 partial[8];
    uint index = xy.y * p.sizes.x + xy.x;
    uint blends = 0, copies = 1;
    float4 result;
    float m = p.props.x;
    uint type = p.sizes.z, direction = p.sizes.w;
    if (type == 3) {
        result = 0;
        copies = 0;
        int2 sa = int2(xy) - p.shifts.xy, sb = int2(xy) - p.shifts.zw;
        if (all(sa >= 0) && sa.x < int(p.sizes.x) && sa.y < int(p.sizes.y)) {
            result = unpack(a[sa.y * p.sizes.x + sa.x]);
            ++blends; ++copies;
        }
        if (all(sb >= 0) && sb.x < int(p.sizes.x) && sb.y < int(p.sizes.y)) {
            result = blend(unpack(b[sb.y * p.sizes.x + sb.x]), result, 0);
            ++blends; ++copies;
        }
    } else if (type == 2) {
        result = m < 0.5f ? mix(unpack(a[index]), p.color, m * 2)
                          : mix(p.color, unpack(b[index]), m * 2 - 1);
    } else {
        float k = m;
        if (type == 1) {
            float2 n = (float2(xy) + 0.5f) / float2(p.sizes.xy);
            float s = direction == 1 ? n.x : direction == 2 ? 1 - n.y : direction == 3 ? n.y : 1 - n.x;
            float soft = p.props.y;
            k = soft > 0 ? clamp((m * (1 + soft) - s) / soft, 0.0f, 1.0f) : float(s < m);
        }
        result = mix(unpack(a[index]), unpack(b[index]), k);
    }
    out[index] = pack(result);
    countWork(counts, uint3(blends, 0, copies), lane, simd, simds, partial, group, groups);
}

kernel void blur_image(device const uchar4* src [[buffer(0)]], device uchar4* dst [[buffer(1)]],
                       constant uint4& p [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {
    uint4 sum = 0;
    int r = int(p.z);
    for (int offset = -r; offset <= r; ++offset) {
        int x = p.w ? clamp(int(xy.x) + offset, 0, int(p.x) - 1) : int(xy.x);
        int y = p.w ? int(xy.y) : clamp(int(xy.y) + offset, 0, int(p.y) - 1);
        sum += uint4(src[y * p.x + x]);
    }
    dst[xy.y * p.x + xy.x] = uchar4(float4(sum) * (1.0f / float(2 * r + 1)) + 0.5f);
}

// A row tile reads each texel once, including its clamped halo. SIMD prefix
// sums turn every box window into two threadgroup reads and a subtraction.
template<uint Tile>
__attribute__((always_inline)) inline
void blurRows(device const uchar4* src, device uchar4* dst, constant uint4& p, uint tid, uint lane, uint simd, uint2 group,
              threadgroup uint4* totals, threadgroup uint4* prefix) {
    uint core = Tile - 2 * p.z;
    int sx = clamp(int(group.x * core + tid) - int(p.z), 0, int(p.x) - 1);
    uint4 sum = simd_prefix_inclusive_sum(uint4(src[group.y * p.x + sx]));
    if (lane + 1 == p.w) totals[simd] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (Tile > 128) {
        // The first SIMD group scans the group totals. A wide tile needs one
        // carry addition per thread instead of a loop over all earlier groups.
        if (simd == 0) {
            uint4 carry = simd_prefix_exclusive_sum(lane < Tile / p.w ? totals[lane] : uint4(0));
            if (lane < Tile / p.w) totals[lane] = carry;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        sum += totals[simd];
    } else {
        for (uint i = 0; i < simd; ++i) sum += totals[i];
    }
    prefix[tid] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint x = group.x * core + tid;
    if (tid >= core || x >= p.x) return;
    sum = prefix[tid + 2 * p.z];
    if (tid) sum -= prefix[tid - 1];
    dst[group.y * p.x + x] = uchar4(float4(sum) * (1.0f / float(2 * p.z + 1)) + 0.5f);
}

kernel void blur_rows(device const uchar4* src [[buffer(0)]], device uchar4* dst [[buffer(1)]],
                      constant uint4& p [[buffer(2)]], uint tid [[thread_index_in_threadgroup]],
                      uint lane [[thread_index_in_simdgroup]], uint simd [[simdgroup_index_in_threadgroup]],
                      uint2 group [[threadgroup_position_in_grid]]) {
    threadgroup uint4 totals[4], prefix[128];
    blurRows<128>(src, dst, p, tid, lane, simd, group, totals, prefix);
}

kernel void blur_rows_wide(device const uchar4* src [[buffer(0)]], device uchar4* dst [[buffer(1)]],
                           constant uint4& p [[buffer(2)]], uint tid [[thread_index_in_threadgroup]],
                           uint lane [[thread_index_in_simdgroup]], uint simd [[simdgroup_index_in_threadgroup]],
                           uint2 group [[threadgroup_position_in_grid]]) {
    threadgroup uint4 totals[16], prefix[512];
    blurRows<512>(src, dst, p, tid, lane, simd, group, totals, prefix);
}

kernel void blur_rows_large(device const uchar4* src [[buffer(0)]], device uchar4* dst [[buffer(1)]],
                           constant uint4& p [[buffer(2)]], uint tid [[thread_index_in_threadgroup]],
                           uint lane [[thread_index_in_simdgroup]], uint simd [[simdgroup_index_in_threadgroup]],
                           uint2 group [[threadgroup_position_in_grid]]) {
    threadgroup uint4 totals[32], prefix[1024];
    blurRows<1024>(src, dst, p, tid, lane, simd, group, totals, prefix);
}

kernel void transpose_image(device const uchar4* src [[buffer(0)]], device uchar4* dst [[buffer(1)]],
                            constant uint2& size [[buffer(2)]], uint2 tid [[thread_position_in_threadgroup]],
                            uint2 group [[threadgroup_position_in_grid]]) {
    threadgroup uchar4 tile[32][33];
    uint2 input = group * 32 + tid;
    for (uint j = 0; j < 32; j += 8)
        if (input.x < size.x && input.y + j < size.y)
            tile[tid.y + j][tid.x] = src[(input.y + j) * size.x + input.x];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint2 output = group.yx * 32 + tid;
    for (uint j = 0; j < 32; j += 8)
        if (output.x < size.y && output.y + j < size.x)
            dst[(output.y + j) * size.y + output.x] = tile[tid.x][tid.y + j];
}

__attribute__((always_inline)) inline uchar4 effectPixel(uchar4 raw, uint2 xy, constant EffectParams& p,
                                                         thread uint& mapped, thread uint& masked) {
    uchar4 result = raw;
    uint type = p.sizes.z;
    float2 pos = float2(xy) + 0.5f;
    if (type >= 7) {
        float k;
        if (type == 7) {
            float x = clamp(min(p.p0.y, float(xy.x) + 1) - max(p.p0.x, float(xy.x)), 0.0f, 1.0f);
            float y = clamp(min(p.p0.w, float(xy.y) + 1) - max(p.p0.z, float(xy.y)), 0.0f, 1.0f);
            k = x * y;
        } else {
            float2 point = pos - p.p0.xy;
            float d;
            if (p.p1.y != 0) {
                float2 radius = max(p.p0.zw, 1e-6f);
                d = (length(point / radius) - 1) * min(radius.x, radius.y);
            } else {
                float2 q = abs(point) - p.p0.zw;
                d = length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f);
            }
            k = clamp(0.5f - d / p.p1.x, 0.0f, 1.0f);
            if (p.p1.z != 0) k = 1 - k;
        }
        if (k < 1) result = uchar4(clamp(float4(raw) * k + 0.5f, 0.0f, 255.0f));
        ++masked;
    } else if (raw.a != 0) {
        float a = float(raw.a) / 255.0f;
        float3 c = float3(raw.rgb) / 255.0f / a;
        bool changed = true;
        switch (type) {
            case 1: c = (c - 0.5f) * p.p0.y + 0.5f + p.p0.x;
                    changed = p.p0.x != 0 || p.p0.y != 1; break;
            case 2: { float l = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
                      c = l + (c - l) * p.p0.x; changed = p.p0.x != 1; break; }
            case 3: c += (1 - 2 * c) * p.p0.x; changed = p.p0.x != 0; break;
            case 4: { float l = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
                      c += (l * p.color.rgb - c) * p.p0.x; changed = p.p0.x != 0; break; }
            case 5: { float d = length(pos - p.p1.xy) / p.p0.z;
                      float t = clamp((d - p.p0.y) / (1 - p.p0.y + 1e-6f), 0.0f, 1.0f);
                      float f = 1 - p.p0.x * t * t * (3 - 2 * t);
                      if (f != 1) c *= f; changed = f != 1; break; }
            case 6: c = float3(dot(c, float3(0.2126f, 0.7152f, 0.0722f)) >= p.p0.x ? 1 : 0); break;
        }
        if (changed || a != 1) {
            result = pack(float4(clamp(c, 0.0f, 1.0f) * a, a));
            ++mapped;
        }
    }
    return result;
}

kernel void effect_image(device const uchar4* src [[buffer(0)]], device uchar4* dst [[buffer(1)]],
                         constant EffectParams* params [[buffer(2)]], constant uint& count [[buffer(3)]],
                         device packed_uint3* counts [[buffer(4)]], uint2 xy [[thread_position_in_grid]],
                         uint lane [[thread_index_in_simdgroup]], uint simd [[simdgroup_index_in_threadgroup]],
                         uint simds [[simdgroups_per_threadgroup]], uint2 group [[threadgroup_position_in_grid]],
                         uint2 groups [[threadgroups_per_grid]]) {
    threadgroup uint3 partial[8];
    uint index = xy.y * params[0].sizes.x + xy.x;
    uchar4 result = src[index];
    uint mapped = 0, masked = 0;
    for (uint i = 0; i < count; ++i) result = effectPixel(result, xy, params[i], mapped, masked);
    dst[index] = result;
    countWork(counts, uint3(mapped, masked, 0), lane, simd, simds, partial, group, groups);
}

)METAL";

constexpr const char* kRectShader = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct RectParams {
    uint4 sizes;
    float4 bounds;
    float4 color;
};

kernel void raster_rect(device uchar4* dst [[buffer(0)]], constant RectParams& p [[buffer(1)]],
                        uint2 xy [[thread_position_in_grid]]) {
    uint2 pos = xy + p.sizes.zw;
    float2 coverage = clamp(min(float2(pos) + 1.0f, p.bounds.zw) - max(float2(pos), p.bounds.xy), 0.0f, 1.0f);
    float cov = coverage.x * coverage.y;
    if (cov <= 0.002f) return;
    uint index = pos.y * p.sizes.x + pos.x;
    float alpha = p.color.a * cov;
    float4 source = float4(p.color.rgb * p.color.a, p.color.a) * cov * 255.0f;
    dst[index] = uchar4(min(255.0f, source + float4(dst[index]) * (1.0f - alpha) + 0.5f));
}

struct ConvexParams {
    uint4 sizes;
    float4 color;
    float4 shape;
    uint4 count;
};

// Area under f(x) = clamp(1 - x, 0, 1): the part of a unit pixel right of x.
float rightArea(float x) { return x <= 0 ? x : x >= 1 ? 0.5f : x - 0.5f * x * x; }

// Exact area of a pixel inside a polygon, as the CPU accumulates it: each
// edge adds its signed height in the pixel's row times the mean part of the
// pixel right of it. Pixels well inside or outside the radii skip the edges.
kernel void raster_convex(device uchar4* dst [[buffer(0)]], constant ConvexParams& p [[buffer(1)]],
                          constant float2* pts [[buffer(2)]], constant float2* blocks [[buffer(3)]],
                          uint2 xy [[thread_position_in_grid]]) {
    uint2 pos = xy + p.sizes.zw;
    float2 origin = float2(pos);
    float d = distance(origin + 0.5f, p.shape.xy);
    if (d - 0.75f >= p.shape.w) return;
    float cov = 1.0f;
    if (d + 0.75f > p.shape.z) {
        float sum = 0;
        // Blocks of 16 edges with their y range skip edges outside this row.
        for (uint block = 0; block * 16 < p.count.x; ++block) {
            if (blocks[block].y <= origin.y || blocks[block].x >= origin.y + 1) continue;
            for (uint e = block * 16; e < min(block * 16 + 16, p.count.x); ++e) {
                float2 a = pts[e] - origin, b = pts[e + 1 == p.count.x ? 0 : e + 1] - origin;
                float dir = 1;
                if (a.y > b.y) {
                    float2 t = a;
                    a = b, b = t;
                    dir = -1;
                }
                if (b.y <= 0 || a.y >= 1 || b.y - a.y <= 1e-6f) continue;
                float dxdy = (b.x - a.x) / (b.y - a.y);
                float top = max(a.y, 0.0f), bottom = min(b.y, 1.0f);
                float xa = a.x + (top - a.y) * dxdy, xb = a.x + (bottom - a.y) * dxdy;
                float mean = abs(xb - xa) < 1e-4f ? clamp(1 - 0.5f * (xa + xb), 0.0f, 1.0f)
                                                  : (rightArea(xb) - rightArea(xa)) / (xb - xa);
                sum += dir * (bottom - top) * mean;
            }
        }
        cov = min(1.0f, abs(sum));
    }
    if (cov <= 0.002f) return;
    uint index = pos.y * p.sizes.x + pos.x;
    float alpha = p.color.a * cov;
    float4 source = float4(p.color.rgb * p.color.a, p.color.a) * cov * 255.0f;
    dst[index] = uchar4(min(255.0f, source + float4(dst[index]) * (1.0f - alpha) + 0.5f));
}
)METAL";


constexpr const char* kShadowShaders = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct ShadowParams {
    uint4 sizes;
    int4 shifts;
    float4 color;
    float4 props;
};
uchar4 shadowPack(float4 c) { return uchar4(clamp(c * 255.0f + 0.5f, 0.0f, 255.0f)); }
kernel void shadow_mask(device const uchar4* src [[buffer(0)]], device uchar4* dst [[buffer(1)]],
                         constant ShadowParams& p [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {
    uint index = xy.y * p.sizes.x + xy.x;
    float alpha = float(src[index].a) / 255.0f * p.props.x;
    dst[index] = shadowPack(float4(p.color.rgb * alpha, alpha));
}
kernel void shadow_composite(device const uchar4* src [[buffer(0)]], device const uchar4* shadow [[buffer(1)]],
                              device uchar4* dst [[buffer(2)]], constant ShadowParams& p [[buffer(3)]],
                              uint2 xy [[thread_position_in_grid]]) {
    uint index = xy.y * p.sizes.x + xy.x;
    int2 shifted = int2(xy) - p.shifts.xy;
    float4 under = 0;
    if (all(shifted >= 0) && shifted.x < int(p.sizes.x) && shifted.y < int(p.sizes.y))
        under = float4(shadow[shifted.y * p.sizes.x + shifted.x]) / 255.0f;
    float4 original = float4(src[index]) / 255.0f;
    dst[index] = shadowPack(original + under * (1.0f - original.a));
}
)METAL";

struct Device {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> coreLibrary;
    id<MTLComputePipelineState> fill, draw, solid, rect, convex, transition, blur, blurRows, blurRowsWide, transpose, effect;
    std::string error;
    std::once_flag rectInit, shadowInit, largeBlurInit;
    id<MTLComputePipelineState> blurRowsLarge;
    id<MTLComputePipelineState> shadowMask, shadowComposite;

    Device() {
        @autoreleasepool {
            device = metalDevice();
            if (!device) {
                error = "no Metal device";
                return;
            }
            queue = [device newCommandQueue];
            NSError* err = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShaders]
                                                          options:nil
                                                            error:&err];
            if (!library || !queue) {
                error = err ? err.localizedDescription.UTF8String : "cannot create Metal queue";
                return;
            }
            coreLibrary = library;
            auto pipeline = [&](NSString* name) -> id<MTLComputePipelineState> {
                id<MTLFunction> fn = [library newFunctionWithName:name];
                return fn ? [device newComputePipelineStateWithFunction:fn error:&err] : nil;
            };
            fill = pipeline(@"fill_image");
            draw = pipeline(@"draw_layer");
            solid = pipeline(@"solid_region");
            transition = pipeline(@"transition_image");
            blur = pipeline(@"blur_image");
            blurRows = pipeline(@"blur_rows");
            blurRowsWide = pipeline(@"blur_rows_wide");
            transpose = pipeline(@"transpose_image");
            effect = pipeline(@"effect_image");
            if (!fill || !draw || !solid || !transition || !blur || !blurRows || !transpose || !effect)
                error = err ? err.localizedDescription.UTF8String : "cannot create Metal pipeline";
        }
    }
    bool available() const { return fill && draw && solid && transition && blur && blurRows && transpose && effect; }

    id<MTLComputePipelineState> largeBlurPipeline() {
        std::call_once(largeBlurInit, [&] {
            @autoreleasepool {
                NSError* error = nil;
                id<MTLFunction> function = [coreLibrary newFunctionWithName:@"blur_rows_large"];
                blurRowsLarge = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
            }
        });
        return blurRowsLarge;
    }

    id<MTLComputePipelineState> rectanglePipeline() {
        std::call_once(rectInit, [&] {
            @autoreleasepool {
                // Most generators stay CPU-side. Compile these optional shaders
                // only when a sufficiently large generator list needs them.
                NSError* err = nil;
                id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kRectShader]
                                                              options:nil error:&err];
                id<MTLFunction> fn = [library newFunctionWithName:@"raster_rect"];
                rect = fn ? [device newComputePipelineStateWithFunction:fn error:&err] : nil;
                fn = [library newFunctionWithName:@"raster_convex"];
                convex = fn ? [device newComputePipelineStateWithFunction:fn error:&err] : nil;
            }
        });
        return rect;
    }

    id<MTLComputePipelineState> convexPipeline() {
        rectanglePipeline();
        return convex;
    }

    bool shadowPipelinesAvailable() {
        std::call_once(shadowInit, [&] {
            @autoreleasepool {
                NSError* err = nil;
                id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShadowShaders]
                                                              options:nil error:&err];
                id<MTLFunction> mask = [library newFunctionWithName:@"shadow_mask"];
                id<MTLFunction> composite = [library newFunctionWithName:@"shadow_composite"];
                shadowMask = mask ? [device newComputePipelineStateWithFunction:mask error:&err] : nil;
                shadowComposite = composite ? [device newComputePipelineStateWithFunction:composite error:&err] : nil;
            }
        });
        return shadowMask && shadowComposite;
    }
};

Device& sharedDevice() {
    static Device d;
    return d;
}

std::array<uint8_t, 4> colorBytes(Color c) {
    auto byte = [](float v) { return uint8_t(std::clamp(v, 0.0f, 255.0f) + 0.5f); };
    return {byte(c.r * c.a * 255), byte(c.g * c.a * 255), byte(c.b * c.a * 255), byte(c.a * 255)};
}

struct DrawParams {
    uint32_t sizes[4];
    int32_t bounds[4];
    float matrix[4];
    float offset[4];
    float props[4];
    float color[4];
};
struct RectParams {
    uint32_t sizes[4];
    float bounds[4];
    float color[4];
};
struct ConvexParams {
    uint32_t sizes[4];
    float color[4];
    float shape[4];
    uint32_t count[4];
};
struct ShadowParams {
    uint32_t sizes[4];
    int32_t shifts[4];
    float color[4];
    float props[4];
};
struct TransitionParams {
    uint32_t sizes[4];
    int32_t shifts[4];
    float props[4];
    float color[4];
};
struct EffectParams {
    uint32_t sizes[4];
    float p0[4], p1[4], color[4];
};
static_assert(sizeof(DrawParams) == 96 && sizeof(RectParams) == 48 && sizeof(ConvexParams) == 64 && sizeof(TransitionParams) == 64 &&
              sizeof(EffectParams) == 64 && sizeof(ShadowParams) == 64);

class MetalFrame final : public GpuFrame {
    struct Surface {
        ImagePtr image;
        id<MTLBuffer> buffer;
        bool privatePixels = false, effectsApplied = false;
        std::shared_ptr<Image> readback;
    };
    struct Stats {
        id<MTLBuffer> buffer;
        size_t groups;
        bool effects = false;
    };
    Device& device_;
    std::vector<Stats> stats_;
    std::unordered_map<const Image*, Surface> surfaces_;
    std::vector<id<MTLBuffer>> leased_;
    // Release the command buffer before the image owners behind no-copy buffers.
    id<MTLCommandBuffer> commands_;
    bool valid_ = true;
    double elapsedSeconds_ = 0;

    id<MTLBuffer> buffer(ImagePtr image, bool initialize = true) {
        if (auto it = surfaces_.find(image.get()); it != surfaces_.end()) return it->second.buffer;
        size_t bytes = image->bytes();
        if (!bytes || bytes > device_.device.maxBufferLength) {
            valid_ = false;
            return nil;
        }
        id<MTLBuffer> result = pixelStorage().buffer(const_cast<uint8_t*>(image->px.data()));
        if (!result) {
            result = initialize ? [device_.device newBufferWithBytes:image->px.data()
                                                              length:bytes
                                                             options:MTLResourceStorageModeShared]
                                : [device_.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        }
        if (!result) valid_ = false;
        else {
            if (initialize && result.contents != image->px.data()) work::addCopy(bytes / 4);
            const Image* key = image.get();
            surfaces_.emplace(key, Surface{std::move(image), result});
        }
        return result;
    }

    MTLSize threadgroupSize(id<MTLComputePipelineState> pipeline) const {
        NSUInteger width = pipeline.threadExecutionWidth;
        return MTLSizeMake(width, std::min<NSUInteger>(8, pipeline.maxTotalThreadsPerThreadgroup / width), 1);
    }

    id<MTLBuffer> counters(id<MTLComputePipelineState> pipeline, int w, int h, bool effects = false) {
        MTLSize size = threadgroupSize(pipeline);
        size_t groups = ((w + size.width - 1) / size.width) * ((h + size.height - 1) / size.height);
        id<MTLBuffer> buffer = privateBuffer(groups * 3 * sizeof(uint32_t));
        if (buffer) stats_.push_back(Stats{buffer, groups, effects});
        return buffer;
    }

    void dispatch(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> pipeline, int w, int h) {
        if (!enc) {
            valid_ = false;
            return;
        }
        [enc setComputePipelineState:pipeline];
        [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:threadgroupSize(pipeline)];
        [enc endEncoding];
    }

    id<MTLBuffer> privateBuffer(size_t bytes) {
        id<MTLBuffer> result = temporaryBuffers().acquire(device_.device, bytes);
        if (!result) valid_ = false;
        else leased_.push_back(result);
        return result;
    }

    id<MTLComputePipelineState> blurPipeline(int radius) const {
        if (radius < 4 || radius > 256) return nil;
        const size_t tile = radius <= 32 ? 128 : radius <= 128 ? 512 : 1024;
        id<MTLComputePipelineState> pipeline = radius <= 32 ? device_.blurRows : radius <= 128 ? device_.blurRowsWide : device_.largeBlurPipeline();
        const size_t lanes = pipeline.threadExecutionWidth;
        return lanes >= 32 && tile % lanes == 0 && pipeline.maxTotalThreadsPerThreadgroup >= tile ? pipeline : nil;
    }

    void transpose(id<MTLBuffer> in, id<MTLBuffer> out, int w, int h) {
        id<MTLComputeCommandEncoder> enc = [commands_ computeCommandEncoder];
        if (!enc) {
            valid_ = false;
            return;
        }
        uint32_t size[2] = {uint32_t(w), uint32_t(h)};
        [enc setComputePipelineState:device_.transpose];
        [enc setBuffer:in offset:0 atIndex:0];
        [enc setBuffer:out offset:0 atIndex:1];
        [enc setBytes:size length:sizeof size atIndex:2];
        [enc dispatchThreadgroups:MTLSizeMake((w + 31) / 32, (h + 31) / 32, 1)
           threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
        [enc endEncoding];
        work::addCopy(uint64_t(w) * h);
    }

    void flushPointEffects(ImagePtr image, std::vector<EffectParams>& pending) {
        if (pending.empty() || !valid_) return;
        Surface& surface = surfaces_.at(image.get());
        id<MTLBuffer> in = surface.buffer;
        id<MTLBuffer> out = surface.privatePixels ? in : privateBuffer(image->bytes());
        id<MTLBuffer> stats = counters(device_.effect, image->w, image->h, true);
        if (!valid_) return;
        uint32_t count = uint32_t(pending.size());
        id<MTLComputeCommandEncoder> enc = [commands_ computeCommandEncoder];
        [enc setBuffer:in offset:0 atIndex:0];
        [enc setBuffer:out offset:0 atIndex:1];
        [enc setBytes:pending.data() length:pending.size() * sizeof(EffectParams) atIndex:2];
        [enc setBytes:&count length:sizeof count atIndex:3];
        [enc setBuffer:stats offset:0 atIndex:4];
        dispatch(enc, device_.effect, image->w, image->h);
        surface.buffer = out;
        surface.privatePixels = true;
        pending.clear();
    }

    void blurSurface(Surface& surface, int w, int h, int r) {
        if (r < 1) return;
        id<MTLBuffer> a = privateBuffer(size_t(w) * h * 4);
        id<MTLBuffer> b = surface.privatePixels ? surface.buffer : privateBuffer(size_t(w) * h * 4);
        if (!valid_) return;
        id<MTLBuffer> in = surface.buffer;
        id<MTLComputePipelineState> pipeline = blurPipeline(r);
        const size_t tile = r <= 32 ? 128 : r <= 128 ? 512 : 1024;
        const size_t lanes = pipeline.threadExecutionWidth;
        const bool prefixRows = pipeline != nil;
        // Transpose larger images so column prefixes also read contiguous
        // memory. Small images avoid the two extra dispatches.
        const bool transposeCols = prefixRows && uint64_t(w) * h >= 65536 &&
                                   device_.transpose.maxTotalThreadsPerThreadgroup >= 256;
        int width = w, height = h;
        for (int pass = 0; pass < 6; ++pass) {
            if (pass == 3 && transposeCols) {
                id<MTLBuffer> out = in == a ? b : a;
                transpose(in, out, width, height);
                if (!valid_) return;
                in = out;
                std::swap(width, height);
            }
            id<MTLBuffer> out = in == a ? b : a;
            const bool horizontal = pass < 3 || transposeCols;
            const bool prefix = prefixRows && horizontal;
            uint32_t params[4] = {uint32_t(width), uint32_t(height), uint32_t(r),
                                  uint32_t(prefix ? lanes : horizontal)};
            id<MTLComputeCommandEncoder> enc = [commands_ computeCommandEncoder];
            if (!enc) {
                valid_ = false;
                return;
            }
            [enc setBuffer:in offset:0 atIndex:0];
            [enc setBuffer:out offset:0 atIndex:1];
            [enc setBytes:params length:sizeof params atIndex:2];
            if (prefix) {
                const size_t groups = (width + tile - 2 * r - 1) / (tile - 2 * r);
                [enc setComputePipelineState:pipeline];
                [enc dispatchThreadgroups:MTLSizeMake(groups, height, 1)
                   threadsPerThreadgroup:MTLSizeMake(tile, 1, 1)];
                [enc endEncoding];
                // Seed each prefix, count scan/carry additions, then the
                // subtraction for every window except the tile's first.
                size_t scanSteps = 0;
                for (size_t stride = 1; stride < lanes; stride *= 2) scanSteps += lanes - stride;
                size_t steps = tile + scanSteps * (tile / lanes);
                if (tile > 128) steps += lanes + scanSteps + tile;
                else {
                    for (size_t simd = 1; simd < tile / lanes; ++simd) steps += simd * lanes;
                }
                work::addBlur(uint64_t(height) * (groups * steps + width - groups));
            } else {
                dispatch(enc, device_.blur, width, height);
                work::addBlur(uint64_t(w) * h * (2 * r + 1));
            }
            in = out;
        }
        if (transposeCols) {
            id<MTLBuffer> out = in == a ? b : a;
            transpose(in, out, width, height);
            if (!valid_) return;
            in = out;
        }
        surface.buffer = in;
        surface.privatePixels = true;
    }

    void dropShadow(Surface& surface, int w, int h, const ParamFn& params, double unit) {
        auto n = [&](std::string_view name, double fallback) { return asNumber(params(name), fallback); };
        Color color = asColor(params("color"), Color::gray(0));
        ShadowParams p{};
        p.sizes[0] = w;
        p.sizes[1] = h;
        p.shifts[0] = int(std::lround(n("x", 6) * unit));
        p.shifts[1] = int(std::lround(n("y", 6) * unit));
        p.color[0] = color.r;
        p.color[1] = color.g;
        p.color[2] = color.b;
        p.props[0] = float(std::clamp(n("opacity", .6), 0.0, 1.0)) * color.a;
        double blurRadius = n("blur", 8) * unit;
        if (p.props[0] == 0) return;
        if (!device_.shadowPipelinesAvailable()) {
            valid_ = false;
            return;
        }
        size_t bytes = size_t(w) * h * 4;
        id<MTLBuffer> source = surface.buffer, mask = privateBuffer(bytes);
        if (!valid_) return;
        id<MTLComputeCommandEncoder> enc = [commands_ computeCommandEncoder];
        [enc setBuffer:source offset:0 atIndex:0];
        [enc setBuffer:mask offset:0 atIndex:1];
        [enc setBytes:&p length:sizeof p atIndex:2];
        dispatch(enc, device_.shadowMask, w, h);
        work::addCopy(uint64_t(w) * h);
        Surface shadow{{}, mask, true};
        int radius = int(std::lround(std::max(0.0, blurRadius) / 2));
        blurSurface(shadow, w, h, radius);
        id<MTLBuffer> out = surface.privatePixels ? source : privateBuffer(bytes);
        if (!valid_) return;
        enc = [commands_ computeCommandEncoder];
        [enc setBuffer:source offset:0 atIndex:0];
        [enc setBuffer:shadow.buffer offset:0 atIndex:1];
        [enc setBuffer:out offset:0 atIndex:2];
        [enc setBytes:&p length:sizeof p atIndex:3];
        dispatch(enc, device_.shadowComposite, w, h);
        uint64_t shifted = uint64_t(std::max<int64_t>(0, int64_t(w) - std::abs(int64_t(p.shifts[0])))) *
                           std::max<int64_t>(0, int64_t(h) - std::abs(int64_t(p.shifts[1])));
        work::addCopy(uint64_t(w) * h + shifted);
        work::addBlend(uint64_t(w) * h);
        surface.buffer = out;
        surface.privatePixels = true;
    }

    void effect(ImagePtr image, const GpuEffect& e, double unit, PixelBox content, std::vector<EffectParams>& pending) {
        Surface& surface = surfaces_.at(image.get());
        auto n = [&](std::string_view name, double fallback) { return asNumber(e.params(name), fallback); };
        int w = image->w, h = image->h;
        if (e.type == "blur") {
            int r = int(std::lround(std::max(0.0, n("radius", 4) * unit) / 2));
            if (r < 1) return;
            flushPointEffects(image, pending);
            if (!valid_) return;
            blurSurface(surface, w, h, r);
            return;
        }
        if (e.type == "drop_shadow") {
            flushPointEffects(image, pending);
            if (!valid_) return;
            dropShadow(surface, w, h, e.params, unit);
            return;
        }
        EffectParams p{};
        p.sizes[0] = w;
        p.sizes[1] = h;
        if (e.type == "brightness_contrast") {
            p.sizes[2] = 1;
            p.p0[0] = n("brightness", 0);
            p.p0[1] = n("contrast", 1);
        } else if (e.type == "saturation") {
            p.sizes[2] = 2;
            p.p0[0] = n("amount", 1);
        } else if (e.type == "invert") {
            p.sizes[2] = 3;
            p.p0[0] = n("mix", 1);
        } else if (e.type == "tint") {
            p.sizes[2] = 4;
            p.p0[0] = n("amount", 0.5);
            Color c = asColor(e.params("color"), Color{1, 0.8f, 0.6f, 1});
            p.color[0] = c.r;
            p.color[1] = c.g;
            p.color[2] = c.b;
            p.color[3] = c.a;
        } else if (e.type == "vignette") {
            p.sizes[2] = 5;
            p.p0[0] = n("amount", 0.5);
            p.p0[1] = 1 - std::clamp(float(n("softness", 0.5)), 0.01f, 1.0f);
            p.p0[2] = std::hypot(content.w / 2.0f, content.h / 2.0f);
            p.p1[0] = content.x + content.w / 2.0f;
            p.p1[1] = content.y + content.h / 2.0f;
        } else if (e.type == "threshold") {
            p.sizes[2] = 6;
            p.p0[0] = n("level", 0.5);
        } else if (e.type == "crop") {
            p.sizes[2] = 7;
            p.p0[0] = content.x + std::clamp(n("left", 0), 0.0, 1.0) * content.w;
            p.p0[1] = content.x + (1 - std::clamp(n("right", 0), 0.0, 1.0)) * content.w;
            p.p0[2] = content.y + std::clamp(n("top", 0), 0.0, 1.0) * content.h;
            p.p0[3] = content.y + (1 - std::clamp(n("bottom", 0), 0.0, 1.0)) * content.h;
        } else if (e.type == "mask") {
            p.sizes[2] = 8;
            p.p0[0] = content.x + content.w / 2.0 + n("x", 0) * unit;
            p.p0[1] = content.y + content.h / 2.0 + n("y", 0) * unit;
            p.p0[2] = std::max(0.0, n("width", 400)) * unit / 2;
            p.p0[3] = std::max(0.0, n("height", 300)) * unit / 2;
            p.p1[0] = std::max(1.0, n("feather", 0) * unit);
            p.p1[1] = asString(e.params("shape")) == "ellipse";
            p.p1[2] = asString(e.params("keep")) == "outside";
        } else return;
        // Exact identity controls preserve valid premultiplied bytes, including
        // partially transparent pixels that otherwise take a color round trip.
        bool identity = (p.sizes[2] == 1 && p.p0[0] == 0 && p.p0[1] == 1) ||
                        (p.sizes[2] == 2 && p.p0[0] == 1) ||
                        (p.sizes[2] >= 3 && p.sizes[2] <= 5 && p.p0[0] == 0);
        if (identity) return;
        pending.push_back(p);
        // setBytes accepts at most 4 KiB. Keep each uniform batch within that
        // limit and bound per-thread counter totals independently of clip size.
        if (pending.size() == 64) flushPointEffects(image, pending);
    }

    void draw(const std::shared_ptr<Image>& dst, ImagePtr src, int w, int h, Color color, const Placement& at,
              float opacity, Blend blend) {
        @autoreleasepool {
            if (!valid_ || dst->empty() || w <= 0 || h <= 0 || opacity <= 0 || at.sx == 0 || at.sy == 0) return;
            double cs = std::cos(at.rotation), sn = std::sin(at.rotation), hw = w / 2.0, hh = h / 2.0;
            double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
            for (double u : {-hw, hw})
                for (double v : {-hh, hh}) {
                    double x = at.cx + u * at.sx * cs - v * at.sy * sn;
                    double y = at.cy + u * at.sx * sn + v * at.sy * cs;
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
            if (!std::isfinite(minX) || !std::isfinite(minY) || !std::isfinite(maxX) || !std::isfinite(maxY)) return;
            int x0 = int(std::clamp(std::floor(minX), 0.0, double(dst->w)));
            int y0 = int(std::clamp(std::floor(minY), 0.0, double(dst->h)));
            int x1 = int(std::clamp(std::ceil(maxX), 0.0, double(dst->w)));
            int y1 = int(std::clamp(std::ceil(maxY), 0.0, double(dst->h)));
            if (x0 >= x1 || y0 >= y1) return;
            double ox = at.cx - hw, oy = at.cy - hh;
            bool aligned = at.rotation == 0 && at.sx == 1 && at.sy == 1 && ox == std::floor(ox) && oy == std::floor(oy);
            DrawParams p{};
            p.sizes[0] = dst->w;
            p.sizes[1] = dst->h;
            p.sizes[2] = w;
            p.sizes[3] = h;
            p.bounds[0] = x0;
            p.bounds[1] = y0;
            p.bounds[2] = x1;
            p.bounds[3] = y1;
            p.matrix[0] = float(cs / at.sx);
            p.matrix[1] = float(sn / at.sx);
            p.matrix[2] = float(-sn / at.sy);
            p.matrix[3] = float(cs / at.sy);
            p.offset[0] = aligned ? float(-ox) : float(hw - 0.5 - (at.cx * cs + at.cy * sn) / at.sx);
            p.offset[1] = aligned ? float(-oy) : float(hh - 0.5 - (-at.cx * sn + at.cy * cs) / at.sy);
            p.props[0] = opacity;
            p.props[1] = float(blend);
            p.props[2] = aligned;
            p.props[3] = !src;
            auto rgba = colorBytes(color);
            for (int c = 0; c < 4; ++c) p.color[c] = rgba[c] / 255.0f;
            if (!src && aligned) {
                if (!rgba[3]) return;
                id<MTLBuffer> out = buffer(dst);
                if (!valid_) return;
                id<MTLComputeCommandEncoder> enc = [commands_ computeCommandEncoder];
                [enc setBuffer:out offset:0 atIndex:0];
                [enc setBytes:&p length:sizeof p atIndex:1];
                dispatch(enc, device_.solid, x1 - x0, y1 - y0);
                uint64_t pixels = uint64_t(x1 - x0) * (y1 - y0);
                if (rgba[3] == 255 && opacity == 1 && blend == Blend::Normal) work::addCopy(pixels);
                else work::addBlend(pixels);
                return;
            }
            id<MTLBuffer> out = buffer(dst), in = src ? buffer(std::move(src)) : out;
            id<MTLBuffer> stats = counters(device_.draw, x1 - x0, y1 - y0);
            if (!valid_) return;
            id<MTLComputeCommandEncoder> enc = [commands_ computeCommandEncoder];
            [enc setBuffer:in offset:0 atIndex:0];
            [enc setBuffer:out offset:0 atIndex:1];
            [enc setBytes:&p length:sizeof p atIndex:2];
            [enc setBuffer:stats offset:0 atIndex:3];
            dispatch(enc, device_.draw, x1 - x0, y1 - y0);
        }
    }

  public:
    explicit MetalFrame(Device& d) : device_(d) {
        commands_ = [d.queue commandBuffer];
        valid_ = commands_ != nil;
    }

    ~MetalFrame() override {
        @autoreleasepool {
            // Also cover destruction after a failed or interrupted finish.
            // Uncommitted commands are discarded before their buffers return.
            if (commands_.status == MTLCommandBufferStatusCommitted || commands_.status == MTLCommandBufferStatusScheduled)
                [commands_ waitUntilCompleted];
            commands_ = nil;
            stats_.clear();
            surfaces_.clear();
            for (id<MTLBuffer> buffer : leased_) temporaryBuffers().recycle(buffer);
        }
    }

    bool supportsBlur(double radius) const override {
        if (!std::isfinite(radius) || radius > 512) return false;
        return radius <= 64 || blurPipeline(int(std::lround(radius / 2))) != nil;
    }

    bool supportsDropShadow(double blurRadius) const override {
        return supportsBlur(blurRadius) && device_.shadowPipelinesAvailable();
    }

    void fill(const std::shared_ptr<Image>& dst, Color color) override {
        @autoreleasepool {
            if (!valid_ || dst->empty()) return;
            id<MTLBuffer> out = buffer(dst, false);
            if (!valid_) return;
            uint32_t sizes[4] = {uint32_t(dst->w), uint32_t(dst->h), 0, 0};
            auto rgba = colorBytes(color);
            id<MTLComputeCommandEncoder> enc = [commands_ computeCommandEncoder];
            [enc setBuffer:out offset:0 atIndex:0];
            [enc setBytes:sizes length:sizeof sizes atIndex:1];
            [enc setBytes:rgba.data() length:rgba.size() atIndex:2];
            dispatch(enc, device_.fill, dst->w, dst->h);
            work::addCopy(uint64_t(dst->w) * dst->h);
        }
    }

    void copyImage(const std::shared_ptr<Image>& dst, ImagePtr src, bool readback) override {
        @autoreleasepool {
            if (!valid_ || dst.get() == src.get()) return;
            if (dst->w != src->w || dst->h != src->h) {
                valid_ = false;
                return;
            }
            if (dst->empty()) return;
            id<MTLBuffer> in = buffer(std::move(src)), out = buffer(dst, false);
            if (!valid_) return;
            if (readback) surfaces_.at(dst.get()).readback = dst;
            id<MTLBlitCommandEncoder> enc = [commands_ blitCommandEncoder];
            if (!enc) {
                valid_ = false;
                return;
            }
            [enc copyFromBuffer:in sourceOffset:0 toBuffer:out destinationOffset:0 size:dst->bytes()];
            [enc endEncoding];
            work::addCopy(dst->bytes() / 4);
        }
    }

    bool rasterize(const std::shared_ptr<Image>& dst, const DrawList& list, float scale) override {
        bool hasShapes = false;
        for (const auto& cmd : list.cmds) {
            if (cmd.type != DrawCmd::Type::Clear && cmd.type != DrawCmd::Type::Rect &&
                cmd.type != DrawCmd::Type::Circle && cmd.type != DrawCmd::Type::Line) return false;
            hasShapes |= cmd.type != DrawCmd::Type::Clear;
        }
        @autoreleasepool {
            if (!valid_ || dst->empty()) return true;
            if (hasShapes && (!device_.rectanglePipeline() || !device_.convexPipeline())) return false;
            // Consecutive shapes share one serial encoder, so each dispatch
            // still sees the pixels of the ones before it.
            id<MTLComputeCommandEncoder> enc = nil;
            auto encoder = [&]() -> id<MTLComputeCommandEncoder> {
                if (enc) return enc;
                id<MTLBuffer> out = buffer(dst);
                if (!valid_) return nil;
                enc = [commands_ computeCommandEncoder];
                if (!enc) valid_ = false;
                else [enc setBuffer:out offset:0 atIndex:0];
                return enc;
            };
            for (const auto& cmd : list.cmds) {
                if (cmd.type == DrawCmd::Type::Clear) {
                    if (enc) [enc endEncoding];
                    enc = nil;
                    fill(dst, cmd.color);
                    continue;
                }
                if (cmd.color.a <= 0) continue;
                if (cmd.type != DrawCmd::Type::Rect) {
                    rasterizeConvex(dst, convexOutline(cmd, scale), cmd.color, encoder);
                } else {
                    float x = cmd.a * scale, y = cmd.b * scale, w = cmd.c * scale, h = cmd.d * scale;
                    float left = std::min(x, x + w), right = std::max(x, x + w);
                    float top = std::min(y, y + h), bottom = std::max(y, y + h);
                    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) ||
                        !std::isfinite(bottom) || left >= right || top >= bottom) continue;
                    int x0 = int(std::clamp(std::floor(left), 0.0f, float(dst->w)));
                    int x1 = int(std::clamp(std::ceil(right), 0.0f, float(dst->w)));
                    int y0 = int(std::clamp(std::floor(top), 0.0f, float(dst->h)));
                    int y1 = int(std::clamp(std::ceil(bottom), 0.0f, float(dst->h)));
                    if (x0 >= x1 || y0 >= y1) continue;
                    RectParams p{{uint32_t(dst->w), uint32_t(dst->h), uint32_t(x0), uint32_t(y0)},
                                 {left, top, right, bottom}, {cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a}};
                    if (!encoder()) break;
                    [enc setComputePipelineState:device_.rect];
                    [enc setBytes:&p length:sizeof p atIndex:1];
                    [enc dispatchThreads:MTLSizeMake(x1 - x0, y1 - y0, 1)
                        threadsPerThreadgroup:threadgroupSize(device_.rect)];
                    work::addRaster(uint64_t(x1 - x0) * (y1 - y0));
                }
                if (!valid_) break;
            }
            if (enc) [enc endEncoding];
        }
        return true;
    }

    // Fills the same convex polygon and pixel box as the CPU's fillPolygon.
    template <class Encoder>
    void rasterizeConvex(const std::shared_ptr<Image>& dst, const std::vector<Vec2>& pts, Color color,
                         Encoder& encoder) {
        if (pts.size() < 3) return;
        float minx = pts[0].x, maxx = minx, miny = pts[0].y, maxy = miny, cx = 0, cy = 0, area = 0;
        for (size_t i = 0; i < pts.size(); ++i) {
            Vec2 a = pts[i], b = pts[(i + 1) % pts.size()];
            if (!std::isfinite(a.x) || !std::isfinite(a.y)) return;
            minx = std::min(minx, a.x), maxx = std::max(maxx, a.x);
            miny = std::min(miny, a.y), maxy = std::max(maxy, a.y);
            cx += a.x, cy += a.y;
            area += a.x * b.y - b.x * a.y;
        }
        if (area == 0) return;
        int x0 = std::max(0, int(std::floor(minx))), y0 = std::max(0, int(std::floor(miny)));
        int x1 = std::min(dst->w, int(std::ceil(maxx)) + 1), y1 = std::min(dst->h, int(std::ceil(maxy)) + 1);
        if (x0 >= x1 || y0 >= y1) return;
        // Radii around the vertex centroid: pixels within the inner one are
        // fully covered, and pixels beyond the outer one are empty.
        cx /= float(pts.size()), cy /= float(pts.size());
        float orientation = area > 0 ? 1 : -1, inner = INFINITY, outer = 0;
        std::vector<Vec2> blocks((pts.size() + 15) / 16, Vec2{INFINITY, -INFINITY});
        for (size_t i = 0; i < pts.size(); ++i) {
            Vec2 a = pts[i], b = pts[(i + 1) % pts.size()];
            Vec2& block = blocks[i / 16];
            block = {std::min({block.x, a.y, b.y}), std::max({block.y, a.y, b.y})};
            float ex = b.x - a.x, ey = b.y - a.y, len = std::hypot(ex, ey);
            if (len > 0) inner = std::min(inner, orientation * (ex * (cy - a.y) - ey * (cx - a.x)) / len);
            outer = std::max(outer, std::hypot(a.x - cx, a.y - cy));
        }
        ConvexParams p{{uint32_t(dst->w), uint32_t(dst->h), uint32_t(x0), uint32_t(y0)},
                       {color.r, color.g, color.b, color.a}, {cx, cy, inner, outer},
                       {uint32_t(pts.size()), 0, 0, 0}};
        id<MTLComputeCommandEncoder> enc = encoder();
        if (!enc) return;
        static_assert(sizeof(Vec2) == 2 * sizeof(float));
        [enc setComputePipelineState:device_.convex];
        [enc setBytes:&p length:sizeof p atIndex:1];
        [enc setBytes:pts.data() length:pts.size() * sizeof(Vec2) atIndex:2];
        [enc setBytes:blocks.data() length:blocks.size() * sizeof(Vec2) atIndex:3];
        [enc dispatchThreads:MTLSizeMake(x1 - x0, y1 - y0, 1) threadsPerThreadgroup:threadgroupSize(device_.convex)];
        work::addRaster(uint64_t(x1 - x0) * (y1 - y0));
    }

    void drawLayer(const std::shared_ptr<Image>& dst, ImagePtr src, const Placement& at, float opacity,
                   Blend blend) override {
        int w = src->w, h = src->h;
        draw(dst, std::move(src), w, h, {}, at, opacity, blend);
    }

    void drawSolid(const std::shared_ptr<Image>& dst, int w, int h, Color color, const Placement& at, float opacity,
                   Blend blend) override {
        draw(dst, {}, w, h, color, at, opacity, blend);
    }

    void applyEffects(ImagePtr image, const std::vector<GpuEffect>& effects, double unit, PixelBox content) override {
        @autoreleasepool {
            if (!valid_ || image->empty()) return;
            buffer(image);
            if (!valid_ || surfaces_.at(image.get()).effectsApplied) return;
            if (content.w <= 0 || content.h <= 0) content = {0, 0, image->w, image->h};
            std::vector<EffectParams> pending;
            pending.reserve(std::min<size_t>(64, effects.size()));
            for (const auto& e : effects) {
                effect(image, e, unit, content, pending);
                if (!valid_) return;
            }
            flushPointEffects(image, pending);
            if (!valid_) return;
            surfaces_.at(image.get()).effectsApplied = true;
        }
    }

    void transition(const std::shared_ptr<Image>& dst, ImagePtr a, ImagePtr b, std::string_view type, double mix,
                    const ParamFn& param) override {
        @autoreleasepool {
            if (!valid_ || dst->empty()) return;
            TransitionParams p{};
            p.sizes[0] = dst->w;
            p.sizes[1] = dst->h;
            p.sizes[2] = type == "wipe" ? 1 : type == "dip" ? 2 : type == "slide" ? 3 : 0;
            std::string dir = asString(param("direction"));
            p.sizes[3] = dir == "right" ? 1 : dir == "up" ? 2 : dir == "down" ? 3 : 0;
            p.props[0] = float(std::clamp(mix, 0.0, 1.0));
            p.props[1] = float(asNumber(param("softness"), 0.02));
            auto rgba = colorBytes(asColor(param("color"), Color::gray(0)));
            for (int c = 0; c < 4; ++c) p.color[c] = rgba[c] / 255.0f;
            int dx = p.sizes[3] == 1 ? -1 : p.sizes[3] >= 2 ? 0 : 1;
            int dy = p.sizes[3] == 2 ? 1 : p.sizes[3] == 3 ? -1 : 0;
            p.shifts[0] = -int(std::lround(dx * p.props[0] * dst->w));
            p.shifts[1] = -int(std::lround(dy * p.props[0] * dst->h));
            p.shifts[2] = int(std::lround(dx * (1 - p.props[0]) * dst->w));
            p.shifts[3] = int(std::lround(dy * (1 - p.props[0]) * dst->h));
            id<MTLBuffer> left = buffer(std::move(a)), right = buffer(std::move(b)), out = buffer(dst, false);
            id<MTLBuffer> stats = counters(device_.transition, dst->w, dst->h);
            if (!valid_) return;
            id<MTLComputeCommandEncoder> enc = [commands_ computeCommandEncoder];
            [enc setBuffer:left offset:0 atIndex:0];
            [enc setBuffer:right offset:0 atIndex:1];
            [enc setBuffer:out offset:0 atIndex:2];
            [enc setBytes:&p length:sizeof p atIndex:3];
            [enc setBuffer:stats offset:0 atIndex:4];
            dispatch(enc, device_.transition, dst->w, dst->h);
        }
    }

    bool finish(const std::shared_ptr<Image>& dst) override {
        @autoreleasepool {
            if (!valid_) return false;
            id<MTLBuffer> out = buffer(dst);
            if (!valid_) return false;
            [commands_ commit];
            [commands_ waitUntilCompleted];
            if (commands_.status != MTLCommandBufferStatusCompleted) return false;
            elapsedSeconds_ = std::max(0.0, commands_.GPUEndTime - commands_.GPUStartTime);
            for (const auto& [image, surface] : surfaces_) {
                if (surface.readback && surface.buffer.contents != surface.readback->px.data()) {
                    std::memcpy(surface.readback->px.data(), surface.buffer.contents, surface.readback->bytes());
                    work::addCopy(surface.readback->bytes() / 4);
                }
            }
            if (out.contents != dst->px.data()) {
                std::memcpy(dst->px.data(), out.contents, dst->bytes());
                work::addCopy(dst->px.size() / 4);
            }
            uint64_t blends = 0, taps = 0, copies = 0, mapped = 0, masked = 0;
            for (const auto& stats : stats_) {
                const auto* counts = static_cast<const uint32_t*>(stats.buffer.contents);
                for (size_t i = 0; i < stats.groups * 3; i += 3) {
                    if (stats.effects) {
                        mapped += counts[i];
                        masked += counts[i + 1];
                    } else {
                        blends += counts[i];
                        taps += counts[i + 1];
                        copies += counts[i + 2];
                    }
                }
            }
            work::addBlend(blends);
            work::addTaps(taps);
            work::addCopy(copies);
            work::addMap(mapped);
            work::addMask(masked);
            return true;
        }
    }
    double elapsedSeconds() const override { return elapsedSeconds_; }
};

} // namespace

namespace detail {
void* allocatePixels(size_t bytes) {
    @autoreleasepool {
        return pixelStorage().allocate(bytes);
    }
}
void freePixels(void* data, size_t) noexcept {
    @autoreleasepool {
        pixelStorage().free(data);
    }
}
} // namespace detail

bool gpuAvailable() { return sharedDevice().available(); }
std::string gpuError() { return sharedDevice().error; }
std::unique_ptr<GpuFrame> createGpuFrame() {
    @autoreleasepool {
        Device& d = sharedDevice();
        return d.available() ? std::make_unique<MetalFrame>(d) : nullptr;
    }
}

} // namespace mf
