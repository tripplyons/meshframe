#include "core/work_counter.hpp"

#include <array>
#include <mutex>
#include <memory>
#include <vector>

namespace mf {

namespace {

std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}

// Every thread's counters, so workCounts() can sum them.
std::vector<std::unique_ptr<WorkCounts>>& registry() {
    static std::vector<std::unique_ptr<WorkCounts>> r;
    return r;
}

}  // namespace

WorkCounts& threadWorkCounts() {
    // The registry owns counters after their originating thread exits, so
    // totals and resets remain valid after temporary media/export workers join.
    static thread_local WorkCounts* counts = [] {
        std::lock_guard<std::mutex> lock(registryMutex());
        registry().push_back(std::make_unique<WorkCounts>());
        return registry().back().get();
    }();
    return *counts;
}

WorkCounts workCounts() {
    std::lock_guard<std::mutex> lock(registryMutex());
    WorkCounts sum;
    for (const auto& c : registry()) {
        sum.blendPixels += c->blendPixels;
        sum.sampleTaps += c->sampleTaps;
        sum.mapPixels += c->mapPixels;
        sum.maskPixels += c->maskPixels;
        sum.blurSteps += c->blurSteps;
        sum.copyPixels += c->copyPixels;
        sum.rasterPixels += c->rasterPixels;
        sum.edgeTests += c->edgeTests;
        sum.glyphPixels += c->glyphPixels;
        sum.distancePixels += c->distancePixels;
        sum.decodeFrames += c->decodeFrames;
        sum.scaledPixels += c->scaledPixels;
        sum.layerBuilds += c->layerBuilds;
        sum.evals += c->evals;
        sum.chunks += c->chunks;
    }
    return sum;
}

void resetWorkCounts() {
    std::lock_guard<std::mutex> lock(registryMutex());
    for (const auto& c : registry()) *c = WorkCounts{};
}

uint64_t WorkCounts::total() const {
    return blendPixels + sampleTaps + mapPixels + maskPixels + blurSteps + copyPixels + rasterPixels + edgeTests +
           glyphPixels + distancePixels + decodeFrames + scaledPixels + layerBuilds + evals + chunks;
}

std::string WorkCounts::toString() const {
    std::string s;
    auto line = [&](const char* name, uint64_t v) {
        s += name;
        s += ' ';
        s += std::to_string(v);
        s += '\n';
    };
    line("blend_pixels", blendPixels);
    line("sample_taps", sampleTaps);
    line("map_pixels", mapPixels);
    line("mask_pixels", maskPixels);
    line("blur_steps", blurSteps);
    line("copy_pixels", copyPixels);
    line("raster_pixels", rasterPixels);
    line("edge_tests", edgeTests);
    line("glyph_pixels", glyphPixels);
    line("distance_pixels", distancePixels);
    line("decode_frames", decodeFrames);
    line("scaled_pixels", scaledPixels);
    line("layer_builds", layerBuilds);
    line("evals", evals);
    line("chunks", chunks);
    line("total", total());
    return s;
}

std::string WorkCounts::toJson() const {
    std::string s = "{";
    auto field = [&](const char* name, uint64_t v, bool last = false) {
        s += '"';
        s += name;
        s += "\":";
        s += std::to_string(v);
        if (!last) s += ',';
    };
    field("blend_pixels", blendPixels);
    field("sample_taps", sampleTaps);
    field("map_pixels", mapPixels);
    field("mask_pixels", maskPixels);
    field("blur_steps", blurSteps);
    field("copy_pixels", copyPixels);
    field("raster_pixels", rasterPixels);
    field("edge_tests", edgeTests);
    field("glyph_pixels", glyphPixels);
    field("distance_pixels", distancePixels);
    field("decode_frames", decodeFrames);
    field("scaled_pixels", scaledPixels);
    field("layer_builds", layerBuilds);
    field("evals", evals);
    field("chunks", chunks);
    field("total", total(), true);
    s += '}';
    return s;
}

}  // namespace mf