#include "engine/frame_renderer.hpp"

#include "core/work_counter.hpp"
#include "render/compositor.hpp"
#include "render/font.hpp"
#include "render/raster.hpp"
#include "render/path.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <numbers>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mf {

namespace {

// Properties that place a finished layer on the canvas. They change where the
// layer lands, never the pixels inside it, so they stay out of the layer key.
bool isPlacementProp(std::string_view name) {
    static constexpr std::string_view kPlacement[] = {"x",        "y",         "scale",  "rotation",
                                                     "opacity",  "blend",     "motionPath",
                                                     "motionProgress", "motionRotate"};
    return std::ranges::any_of(kPlacement, [&](std::string_view p) { return p == name; });
}

bool gpuEffectsFor(Evaluator& ev, const Clip& c, double unit, const GpuFrame& gpu) {
    if (c.kind != ClipKind::Media && c.kind != ClipKind::Generator && c.kind != ClipKind::Text && c.kind != ClipKind::Path) return false;
    bool any = false;
    for (const auto& e : c.videoEffects) {
        if (!e.enabled) continue;
        any = true;
        if (e.type == "blur") {
            if (!gpu.supportsBlur(asNumber(ev.effectParams(e, false)("radius"), 4) * unit)) return false;
        } else if (e.type == "drop_shadow") {
            if (!gpu.supportsDropShadow(asNumber(ev.effectParams(e, false)("blur"), 8) * unit)) return false;
        } else if (e.type != "brightness_contrast" && e.type != "saturation" && e.type != "invert" &&
                   e.type != "tint" && e.type != "vignette" && e.type != "crop" && e.type != "mask") {
            return false;
        }
    }
    return any;
}

void appendNumber(std::string& key, double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    key += buf;
    key.push_back('|');
}

void appendValue(std::string& key, const Value& v) {
    switch (typeOf(v)) {
        case ValueType::Number: appendNumber(key, asNumber(v)); break;
        case ValueType::Color: {
            Color c = asColor(v);
            appendNumber(key, c.r), appendNumber(key, c.g), appendNumber(key, c.b), appendNumber(key, c.a);
            break;
        }
        case ValueType::String: key += asString(v); key.push_back('\x1f'); break;
        case ValueType::None: key.push_back('\x1e'); break;
    }
}

// Everything a clip's own pixels depend on: its identity and timing, the
// project frame it is rasterized for, every property that feeds its content or
// its effects, and the media file behind it. Two reads that produce the same
// key must produce the same image, so a clip that did not change is rendered
// once instead of once per frame.
std::string layerKey(Evaluator& ev, const Clip& c, double unit, int w, int h, double t) {
    const Project& p = ev.project();
    std::string key;
    key.reserve(512);
    appendNumber(key, c.id);
    appendNumber(key, double(c.kind));
    appendNumber(key, c.start);
    appendNumber(key, c.duration);
    appendNumber(key, c.in);
    appendNumber(key, c.speed);
    appendNumber(key, c.media);
    appendNumber(key, unit);
    appendNumber(key, w);
    appendNumber(key, h);
    appendNumber(key, p.settings.width);
    appendNumber(key, p.settings.height);
    appendNumber(key, p.settings.fps);
    // A generator reads the clock from its script, so it never reuses a layer.
    // A media clip picks its source frame from the clock.
    if (c.kind == ClipKind::Media) appendNumber(key, t);
    if (const MediaSource* m = p.findMedia(c.media)) {
        key += m->path;
        key.push_back('\x1f');
        appendNumber(key, m->width);
        appendNumber(key, m->height);
        appendNumber(key, m->fps);
        appendNumber(key, m->duration);
        appendNumber(key, double(m->kind));
        key.push_back(m->hasVideo ? 'v' : '-');
    }
    for (const auto& [name, prop] : c.props) {
        if (isPlacementProp(name)) continue;
        key += name;
        key.push_back('\x1f');
        appendValue(key, ev.prop(c, name));
    }
    for (const auto& e : c.videoEffects) {
        appendNumber(key, e.id);
        key += e.type;
        key.push_back('\x1f');
        key.push_back(e.enabled ? '1' : '0');
        if (!e.enabled) continue;
        for (const auto& [name, prop] : e.props) {
            key += name;
            key.push_back('\x1f');
            appendValue(key, ev.effectProp(e, name));
        }
    }
    return key;
}

// A rendered layer kept between frames, along with the key that produced it.
struct CachedLayer {
    Id clip = 0;
    std::string key;
    std::shared_ptr<Image> image;
    double offsetX = 0, offsetY = 0, displayW = 0, displayH = 0;
    bool opaque = false;
    PixelBox content{};
};

std::mutex& layerCacheMutex() {
    static std::mutex m;
    return m;
}

// Recent first, so the layers still in use survive a burst of misses.
std::vector<CachedLayer>& layerCache() {
    static std::vector<CachedLayer> cache;
    return cache;
}

constexpr size_t kLayerCacheSize = 16;

void cacheLayer(CachedLayer layer) {
    std::lock_guard lock(layerCacheMutex());
    auto& cache = layerCache();
    std::erase_if(cache, [&](const CachedLayer& entry) { return entry.clip == layer.clip; });
    cache.insert(cache.begin(), std::move(layer));
    if (cache.size() > kLayerCacheSize) cache.resize(kLayerCacheSize);
}

// Whether every pixel of img has alpha 255.
bool fullyOpaque(const Image& img) {
    for (size_t i = 3; i < img.px.size(); i += 4)
        if (img.px[i] != 255) return false;
    return true;
}

// Small shapes take less CPU time than a Metal dispatch. Estimate clipped
// pixels per command so generator offloading amortizes that fixed cost.
bool gpuGeneratorWorthwhile(const DrawList& list, float scale, int w, int h) {
    uint64_t pixels = 0;
    for (const auto& cmd : list.cmds) {
        if (cmd.type == DrawCmd::Type::Clear) {
            pixels += uint64_t(w) * h;
        } else if (cmd.type == DrawCmd::Type::Rect) {
            if (cmd.color.a <= 0) continue;
            float x = cmd.a * scale, y = cmd.b * scale, rw = cmd.c * scale, rh = cmd.d * scale;
            float left = std::min(x, x + rw), right = std::max(x, x + rw);
            float top = std::min(y, y + rh), bottom = std::max(y, y + rh);
            if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) || !std::isfinite(bottom)) continue;
            int x0 = int(std::clamp(std::floor(left), 0.0f, float(w)));
            int x1 = int(std::clamp(std::ceil(right), 0.0f, float(w)));
            int y0 = int(std::clamp(std::floor(top), 0.0f, float(h)));
            int y1 = int(std::clamp(std::ceil(bottom), 0.0f, float(h)));
            pixels += uint64_t(x1 - x0) * (y1 - y0);
        } else if (cmd.type == DrawCmd::Type::Circle || cmd.type == DrawCmd::Type::Line) {
            if (cmd.color.a <= 0) continue;
            auto pts = convexOutline(cmd, scale);
            if (pts.empty()) continue;
            float minx = pts[0].x, maxx = minx, miny = pts[0].y, maxy = miny;
            for (auto p : pts) {
                minx = std::min(minx, p.x), maxx = std::max(maxx, p.x);
                miny = std::min(miny, p.y), maxy = std::max(maxy, p.y);
            }
            if (!std::isfinite(minx) || !std::isfinite(maxx) || !std::isfinite(miny) || !std::isfinite(maxy)) continue;
            float x0 = std::clamp(std::floor(minx), 0.0f, float(w)), x1 = std::clamp(std::ceil(maxx) + 1, 0.0f, float(w));
            float y0 = std::clamp(std::floor(miny), 0.0f, float(h)), y1 = std::clamp(std::ceil(maxy) + 1, 0.0f, float(h));
            pixels += uint64_t(x1 - x0) * uint64_t(y1 - y0);
        } else return false;
    }
    return !list.cmds.empty() && pixels >= uint64_t(list.cmds.size()) * 65536;
}

// Draws a text clip's lines with their alignment, over an outline when it
// has one. `unit` converts project pixels to image pixels.
std::shared_ptr<Image> renderText(Evaluator& ev, const Clip& c, Font& font, double unit) {
    TextLayout layout;
    layout.px = float(ev.num(c, "size", 96) * unit);
    if (layout.px < 1) return nullptr;
    layout.wrap = float(std::max(0.0, ev.num(c, "wrap", 0)) * unit);
    layout.lineSpacing = float(ev.num(c, "lineHeight", 1));
    layout.outline = float(std::max(0.0, ev.num(c, "outline", 0)) * unit);
    TextBlock b = layoutTextBlock(font, ev.str(c, "text"), layout);
    if (b.lines.empty()) return nullptr;
    std::string align = ev.str(c, "align");
    auto img = std::make_shared<Image>(b.w, b.h);
    Color color = ev.color(c, "color", Color::gray(1));
    for (size_t i = 0; i < b.lines.size(); ++i) {
        float inner = b.w - 2 * b.margin;
        float x = b.margin + (align == "left" ? 0 : align == "right" ? inner - b.widths[i] : (inner - b.widths[i]) / 2);
        drawText(*img, font, b.lines[i], x, b.margin + b.lineHeight * float(i), layout.px, color);
    }
    if (layout.outline > 0) {
        Image under = outlineOf(*img, layout.outline, ev.color(c, "outlineColor", Color::gray(0)));
        drawLayer(under, *img, Placement{under.w / 2.0, under.h / 2.0}, 1, Blend::Normal);
        *img = std::move(under);
    }
    return img;
}

}  // namespace

FrameRenderer::Layer FrameRenderer::content(Evaluator& ev, const Clip& c, double t, double unit, int w, int h,
                                            bool inspectOpacity, GpuFrame* rasterGpu) {
    work::addLayer(1);
    const Project& p = ev.project();
    Layer layer;
    bool opaque = false;
    bool solidLayer = false;
    Color solidColor;
    switch (c.kind) {
        case ClipKind::Media: {
            const MediaSource* m = p.findMedia(c.media);
            if (!m || !m->hasVideo || m->width <= 0) break;
            double src = c.in + (t - c.start) * c.speed;
            double frameDur = 1.0 / (m->fps > 0 ? m->fps : 30);
            src = std::clamp(src, 0.0, std::max(0.0, m->duration - frameDur));
            // Contain fits inside the frame and cover fills it, both keeping the
            // aspect; stretch fills it exactly; none keeps one media pixel per
            // project pixel.
            std::string fit = ev.str(c, "fit");
            double sx = double(w) / m->width, sy = double(h) / m->height;
            if (fit == "stretch") {
            } else if (fit == "none") {
                sx = sy = unit;
            } else {
                sx = sy = fit == "cover" ? std::max(sx, sy) : std::min(sx, sy);
            }
            layer.displayW = m->width * sx;
            layer.displayH = m->height * sy;
            ImagePtr frame = cache_.frame(*m, src, int(std::ceil(layer.displayW)), int(std::ceil(layer.displayH)));
            if (frame) {
                if (rasterGpu) {
                    layer.image = std::make_shared<Image>(Image::forOverwrite(frame->w, frame->h));
                    rasterGpu->copyImage(layer.image, frame);
                    layer.gpuPixels = true;
                } else layer.image = std::make_shared<Image>(*frame);
                // Only the CPU's deferred background fill needs this scan.
                opaque = inspectOpacity && fullyOpaque(*layer.image);
            }
            break;
        }
        case ClipKind::Text: {
            std::string fontPath = ev.str(c, "font");
            auto font = fontPath.empty() ? Font::sans() : Font::load(fontPath);
            if (!font) {
                ev.reportRuntimeError(fmt::format("clip:{}", c.id), "cannot load font " + fontPath);
                font = Font::sans();
            }
            if (!font) break;
            layer.image = renderText(ev, c, *font, unit);
            if (layer.image) {
                layer.displayW = layer.image->w;
                layer.displayH = layer.image->h;
            }
            break;
        }
        case ClipKind::Solid: {
            Color color = ev.color(c, "color", Color::gray(0.5f));
            layer.displayW = w;
            layer.displayH = h;
            opaque = uint8_t(std::clamp(color.a, 0.0f, 1.0f) * 255 + 0.5f) == 255;
            // The pixels are only built when an effect needs them; otherwise the
            // colour is composited straight from these four bytes.
            solidLayer = true;
            solidColor = color;
            break;
        }
        case ClipKind::Generator: {
            DrawList dl;
            std::string err;
            std::string key = fmt::format("clip:{}", c.id);
            if (!ev.drawGenerator(c, dl, &err)) {
                ev.reportRuntimeError(key, err);
                break;
            }
            ev.clearRuntimeError(key);
            bool clearsCanvas = !dl.cmds.empty() && dl.cmds.front().type == DrawCmd::Type::Clear;
            layer.image = std::make_shared<Image>(clearsCanvas ? Image::forOverwrite(w, h) : Image(w, h));
            layer.gpuPixels = rasterGpu && gpuGeneratorWorthwhile(dl, float(unit), w, h) &&
                              rasterGpu->rasterize(layer.image, dl, float(unit));
            if (!layer.gpuPixels) drawList(*layer.image, dl, float(unit));
            layer.displayW = w;
            layer.displayH = h;
            break;
        }
        case ClipKind::Path: {
            const std::string key = fmt::format("clip:{}:path", c.id);
            try {
                double width = ev.num(c, "width", 240), height = ev.num(c, "height", 160);
                double strokeWidth = ev.num(c, "strokeWidth");
                double miterLimit = ev.num(c, "strokeMiterLimit", 4);
                if (!std::isfinite(miterLimit) || miterLimit < 1)
                    throw std::runtime_error("strokeMiterLimit must be finite and at least 1");
                if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(strokeWidth) ||
                    width < 0 || height < 0 || strokeWidth < 0)
                    throw std::runtime_error("path dimensions must be finite and nonnegative");
                std::string data = ev.str(c, "path");
                std::string shape = ev.str(c, "shape");
                VectorPath path = data.empty() ? (shape == "path" ? VectorPath{} : shapePath(shape, width, height)) : parsePath(data);
                double morph = ev.num(c, "morph");
                double trimStart = ev.num(c, "trimStart"), trimEnd = ev.num(c, "trimEnd", 1);
                if (!std::isfinite(morph) || !std::isfinite(trimStart) || !std::isfinite(trimEnd))
                    throw std::runtime_error("morph and trim progress must be finite");
                std::string target = ev.str(c, "targetPath");
                if (!target.empty()) path = morphPath(path, parsePath(target), morph);
                double minX = std::numeric_limits<double>::infinity(), minY = minX;
                double maxX = -minX, maxY = -minX;
                for (const auto& contour : path.contours) for (const auto& point : contour.points) {
                    if (!std::isfinite(point.x) || !std::isfinite(point.y)) throw std::runtime_error("nonfinite path coordinate");
                    minX = std::min(minX, point.x);
                    maxX = std::max(maxX, point.x);
                    minY = std::min(minY, point.y);
                    maxY = std::max(maxY, point.y);
                }
                ev.clearRuntimeError(key);
                if (!std::isfinite(minX)) break;
                // Keep geometry coordinates relative to its origin, while allocating only its bounds.
                double centerX = (minX + maxX) / 2, centerY = (minY + maxY) / 2;
                double strokeFactor = ev.str(c, "strokeLinejoin") == "miter" ? miterLimit : 1;
                if (ev.str(c, "strokeLinecap") == "square") strokeFactor = std::max(strokeFactor, std::sqrt(2.0));
                double strokeExtent = strokeWidth * strokeFactor;
                double pw = std::ceil((maxX - minX + strokeExtent) * unit) + 4;
                double ph = std::ceil((maxY - minY + strokeExtent) * unit) + 4;
                if (!std::isfinite(pw) || !std::isfinite(ph) || pw > 16384 || ph > 16384 || pw * ph > 64000000)
                    throw std::runtime_error("path raster exceeds 16384 pixels per side or 64 million pixels");
                for (auto& contour : path.contours) for (auto& point : contour.points) {
                    point.x -= centerX;
                    point.y -= centerY;
                }
                layer.image = std::make_shared<Image>(std::max(1, int(pw)), std::max(1, int(ph)));
                drawPath(*layer.image, path, unit, ev.color(c, "fill", Color::gray(1)),
                         ev.color(c, "stroke", Color::gray(1)), strokeWidth,
                         trimStart, trimEnd, ev.str(c, "fillRule"),
                         ev.str(c, "strokeLinecap"), ev.str(c, "strokeLinejoin"), miterLimit);
                layer.displayW = layer.image->w;
                layer.displayH = layer.image->h;
                layer.offsetX = centerX * unit;
                layer.offsetY = centerY * unit;
            } catch (const std::exception& e) {
                ev.reportRuntimeError(key, e.what());
                layer.image.reset();
            }
            break;
        }
        case ClipKind::Synth: break;
    }
    layer.opaque = opaque;
    layer.solid = solidLayer;
    layer.color = solidColor;
    return layer;
}

bool FrameRenderer::drawClip(Evaluator& ev, const std::shared_ptr<Image>& target, const Clip& c, double t, double unit,
                             bool useBlend, GpuFrame* gpu, int padX, int padY) {
    Image& dst = *target;
    const int layerW = dst.w - 2 * padX, layerH = dst.h - 2 * padY;
    // A generator's pixels come straight out of its script, so it is rebuilt
    // every frame; every other kind is reused while its key holds.
    const bool reusable = c.kind != ClipKind::Generator;
    const bool gpuEffects = gpu && gpuEffectsFor(ev, c, unit, *gpu);
    std::string key;
    if (reusable) {
        key = layerKey(ev, c, unit, layerW, layerH, t);
        key.push_back(gpuEffects ? 'g' : 'c');
    }

    Layer layer;
    bool hit = false;
    bool alphaKept = false;
    if (reusable) {
        std::lock_guard lock(layerCacheMutex());
        std::vector<CachedLayer>& cache = layerCache();
        for (size_t i = 0; i < cache.size(); ++i) {
            if (cache[i].clip != c.id || cache[i].key != key) continue;
            layer.image = cache[i].image;
            layer.offsetX = cache[i].offsetX, layer.offsetY = cache[i].offsetY;
            layer.displayW = cache[i].displayW, layer.displayH = cache[i].displayH;
            alphaKept = cache[i].opaque;
            layer.effectContent = cache[i].content;
            hit = true;
            if (i) {
                CachedLayer found = std::move(cache[i]);
                cache.erase(cache.begin() + i);
                cache.insert(cache.begin(), std::move(found));
            }
            break;
        }
    }
    if (!layer.image) {
        bool wantsPixels = false;
        for (const auto& e : c.videoEffects)
            if (e.enabled) wantsPixels = true;
        // CPU effects need the generator's bytes immediately. Keep the entire
        // raster/effect chain on one side of the CPU/GPU boundary.
        GpuFrame* rasterGpu = gpu && (gpuEffects || !wantsPixels) ? gpu : nullptr;
        layer = content(ev, c, t, unit, layerW, layerH, !gpu, rasterGpu);
        bool solid = layer.solid;
        Color solidColor = layer.color;
        if (wantsPixels && !layer.image) {
            // A solid only needs its pixels when an effect will read them.
            layer.image = std::make_shared<Image>(Image::forOverwrite(int(layer.displayW), int(layer.displayH)));
            fill(*layer.image, solidColor);
        }
        if (!layer.image) {
            if (!solid) return false;
        } else if (layer.image->empty()) {
            return false;
        }
        alphaKept = layer.opaque;
        for (const auto& e : c.videoEffects)
            if (e.enabled && (e.type == "mask" || e.type == "crop" || e.type == "drop_shadow")) alphaKept = false;
        if (!layer.image) {
            // No effects means no margin, no padding and no effect pass.
        } else {
        double toDisplay = layer.displayW / layer.image->w;
        double effectUnit = unit / toDisplay;
        // Room for effects that draw past the edges, such as a drop shadow.
        int margin = 0;
        for (const auto& e : c.videoEffects)
            if (e.enabled) {
                margin += effectMargin(e.type, ev.effectParams(e, false), effectUnit);
                if (c.kind == ClipKind::Path && e.type == "blur") {
                    double radius = asNumber(ev.effectParams(e, false)("radius"), 4);
                    double extent = std::max(0.0, radius) * effectUnit * 2;
                    if (!std::isfinite(radius) || !std::isfinite(extent) || extent > 16384) {
                        ev.reportRuntimeError(fmt::format("clip:{}:path", c.id), "path blur exceeds raster limits");
                        return false;
                    }
                    margin += int(std::ceil(extent)) + 1;
                }
            }
        if (c.kind == ClipKind::Path) {
            double paddedW = layer.image->w + 2.0 * margin, paddedH = layer.image->h + 2.0 * margin;
            if (paddedW > 16384 || paddedH > 16384 || paddedW * paddedH > 64000000) {
                ev.reportRuntimeError(fmt::format("clip:{}:path", c.id), "path effects exceed raster limits");
                return false;
            }
        }
        PixelBox content{margin, margin, layer.image->w, layer.image->h};
        layer.effectContent = content;
        if (margin > 0) {
            const int paddedW = layer.image->w + 2 * margin, paddedH = layer.image->h + 2 * margin;
            auto padded = std::make_shared<Image>(layer.gpuPixels ? Image::forOverwrite(paddedW, paddedH)
                                                                : Image(paddedW, paddedH));
            if (layer.gpuPixels) {
                gpu->fill(padded, Color::gray(0, 0));
                // The source's bytes await GPU execution. Copy them into
                // the zeroed halo on the GPU before the effect reads them.
                gpu->drawLayer(padded, layer.image, Placement{padded->w / 2.0, padded->h / 2.0}, 1, Blend::Normal);
            } else {
                for (int y = 0; y < layer.image->h; ++y)
                    std::copy_n(layer.image->row(y), layer.image->w * 4, padded->at(margin, margin + y));
            }
            layer.displayW *= double(padded->w) / layer.image->w;
            layer.displayH *= double(padded->h) / layer.image->h;
            layer.image = std::move(padded);
        }
        for (const auto& e : c.videoEffects) {
            if (!e.enabled || gpuEffects) continue;
            auto params = ev.effectParams(e, false);
            if (c.kind == ClipKind::Path && e.type == "mask") {
                // Mask x/y use the object's origin, even when the path bounds are off-center.
                ParamFn local = [&](std::string_view name) -> Value {
                    if (name == "x") return asNumber(params(name)) - layer.offsetX / unit;
                    if (name == "y") return asNumber(params(name)) - layer.offsetY / unit;
                    return params(name);
                };
                applyEffect(*layer.image, e.type, local, effectUnit, content);
            } else applyEffect(*layer.image, e.type, params, effectUnit, content);
        }
        }
    }  // rebuilt from scratch
    if (layer.image && layer.image->empty()) return false;
    // GPU-produced bytes remain pending until finish; cache CPU-ready images.
    const bool cacheGpuEffects = gpuEffects && (c.kind == ClipKind::Text || c.kind == ClipKind::Path);
    if (reusable && !hit && layer.image && !layer.gpuPixels && !cacheGpuEffects) {
        cacheLayer(CachedLayer{c.id, std::move(key), layer.image, layer.offsetX, layer.offsetY,
                              layer.displayW, layer.displayH, alphaKept, layer.effectContent});
    }
    if (gpuEffects && layer.image && !(cacheGpuEffects && hit)) {
        std::vector<GpuEffect> effects;
        for (const auto& e : c.videoEffects) if (e.enabled) {
            ParamFn params = ev.effectParams(e, false);
            if (c.kind == ClipKind::Path && e.type == "mask") {
                params = [params, x = layer.offsetX / unit, y = layer.offsetY / unit](std::string_view name) -> Value {
                    if (name == "x") return asNumber(params(name)) - x;
                    if (name == "y") return asNumber(params(name)) - y;
                    return params(name);
                };
            }
            effects.push_back({e.type, std::move(params)});
        }
        double effectUnit = unit * layer.image->w / layer.displayW;
        gpu->applyEffects(layer.image, effects, effectUnit, layer.effectContent);
        if (cacheGpuEffects) {
            auto processed = std::make_shared<Image>(Image::forOverwrite(layer.image->w, layer.image->h));
            gpu->copyImage(processed, layer.image, true);
            layer.image = std::move(processed);
            // Publish only completed pixels. Other renderers share this cache.
            pendingGpuLayers_.push_back([entry = CachedLayer{c.id, std::move(key), layer.image, layer.offsetX, layer.offsetY,
                                                            layer.displayW, layer.displayH, alphaKept, layer.effectContent}] {
                cacheLayer(entry);
            });
        }
    }
    double scale = ev.num(c, "scale", 1);
    Placement at;
    double motionX = 0, motionY = 0, motionAngle = 0;
    const std::string motionKey = fmt::format("clip:{}:motionPath", c.id);
    try {
        std::string data = ev.str(c, "motionPath");
        if (!data.empty()) {
            VectorPath path = parsePath(data);
            double progress = ev.num(c, "motionProgress");
            double rotate = ev.num(c, "motionRotate");
            if (!std::isfinite(progress) || !std::isfinite(rotate))
                throw std::runtime_error("motion progress and orientation must be finite");
            auto point = pointOnPath(path, progress);
            motionX = point.x;
            motionY = point.y;
            if (rotate >= 0.5) motionAngle = pathAngle(path, progress);
        }
        ev.clearRuntimeError(motionKey);
    } catch (const std::exception& e) {
        ev.reportRuntimeError(motionKey, e.what());
        return false;
    }
    at.rotation = (ev.num(c, "rotation") + motionAngle) * std::numbers::pi / 180;
    double ox = layer.offsetX * scale, oy = layer.offsetY * scale;
    at.cx = dst.w / 2.0 + (ev.num(c, "x") + motionX) * unit + ox * std::cos(at.rotation) - oy * std::sin(at.rotation);
    at.cy = dst.h / 2.0 + (ev.num(c, "y") + motionY) * unit + ox * std::sin(at.rotation) + oy * std::cos(at.rotation);
    // A solid layer has no image, so its drawn size is its display size times scale.
    int solidW = layer.image ? layer.image->w : int(layer.displayW);
    int solidH = layer.image ? layer.image->h : int(layer.displayH);
    at.sx = scale * layer.displayW / solidW;
    at.sy = scale * layer.displayH / solidH;
    float opacity = float(std::clamp(ev.num(c, "opacity", 1), 0.0, 1.0));
    Blend mode = useBlend ? parseBlend(ev.str(c, "blend")) : Blend::Normal;
    if (useBlend && fillBackground_) {
        fillBackground_ = false;
        // drawLayer's nearest-neighbour path takes the source alpha verbatim, so
        // an opaque layer there reaches blendPixel with sa exactly 1: the normal
        // blend then discards the destination and writes the source colour, and
        // the background underneath cannot show. A bilinear draw interpolates
        // its alpha and lands just under 1, so it still needs the background.
        double halfW = solidW * at.sx / 2.0, halfH = solidH * at.sy / 2.0;
        double ox = at.cx - halfW, oy = at.cy - halfH;
        bool covers = at.cx - halfW <= 0 && at.cy - halfH <= 0 && at.cx + halfW >= dst.w && at.cy + halfH >= dst.h;
        bool exact = at.rotation == 0 && at.sx == 1 && at.sy == 1 && ox == std::floor(ox) && oy == std::floor(oy);
        if (!(alphaKept && covers && exact && opacity >= 1 && mode == Blend::Normal)) fill(dst, background_);
    }
    if (gpu) {
        if (layer.image) gpu->drawLayer(target, layer.image, at, opacity, mode);
        else gpu->drawSolid(target, solidW, solidH, layer.color, at, opacity, mode);
    } else if (layer.image) drawLayer(dst, *layer.image, at, opacity, mode, alphaKept);
    else if (!drawSolidLayer(dst, at, solidW, solidH, layer.color, opacity, mode)) {
        // The placement resamples between texels, so build the pixels after all.
        auto image = std::make_shared<Image>(solidW, solidH);
        fill(*image, layer.color);
        drawLayer(dst, *image, at, opacity, mode, alphaKept);
    }
    return true;
}

std::shared_ptr<Image> FrameRenderer::renderClip(Evaluator& ev, const Clip& c, double t, double scale, int padX, int padY) {
    pendingGpuLayers_.clear();
    ev.setTime(t);
    fillBackground_ = false;  // a clip preview is drawn over transparency
    const auto& st = ev.project().settings;
    int w = std::max(2, int(std::lround(st.width * scale))), h = std::max(2, int(std::lround(st.height * scale)));
    auto gpu = backend_ != RenderBackend::Cpu && (backend_ == RenderBackend::Metal || int64_t(w) * h >= 262144)
                   ? createGpuFrame() : nullptr;
    auto canvas = std::make_shared<Image>(gpu ? Image::forOverwrite(w + 2 * padX, h + 2 * padY)
                                            : Image(w + 2 * padX, h + 2 * padY));
    if (gpu) gpu->fill(canvas, Color{0, 0, 0, 0});
    drawClip(ev, canvas, c, t, double(w) / st.width, false, gpu.get(), padX, padY);
    usedGpu_ = gpu && gpu->finish(canvas);
    gpuSeconds_ = usedGpu_ ? gpu->elapsedSeconds() : 0;
    if (usedGpu_) for (auto& publish : pendingGpuLayers_) publish();
    pendingGpuLayers_.clear();
    if (gpu && !usedGpu_) {
        canvas = std::make_shared<Image>(w + 2 * padX, h + 2 * padY);
        drawClip(ev, canvas, c, t, double(w) / st.width, false, nullptr, padX, padY);
    }
    ev.refreshOutputs();
    return canvas;
}

std::shared_ptr<Image> FrameRenderer::render(Evaluator& ev, double t, int w, int h) {
    pendingGpuLayers_.clear();
    auto gpu = backend_ != RenderBackend::Cpu && (backend_ == RenderBackend::Metal || int64_t(w) * h >= 262144)
                   ? createGpuFrame() : nullptr;
    auto canvas = renderFrame(ev, t, w, h, gpu.get());
    usedGpu_ = gpu && gpu->finish(canvas);
    gpuSeconds_ = usedGpu_ ? gpu->elapsedSeconds() : 0;
    if (usedGpu_) for (auto& publish : pendingGpuLayers_) publish();
    pendingGpuLayers_.clear();
    return gpu && !usedGpu_ ? renderFrame(ev, t, w, h, nullptr) : canvas;
}

std::shared_ptr<Image> FrameRenderer::renderFrame(Evaluator& ev, double t, int w, int h, GpuFrame* gpu) {
    ev.setTime(t);
    const Project& p = ev.project();
    double unit = double(w) / p.settings.width;
    auto canvas = std::make_shared<Image>(gpu ? Image::forOverwrite(w, h) : Image(w, h));
    // CPU draws defer the background until the first layer's coverage is known.
    // Metal initializes every canvas pixel before compositing.
    background_ = p.settings.background;
    fillBackground_ = !gpu;
    if (gpu) gpu->fill(canvas, background_);

    for (const auto& track : p.tracks) {
        if (track.kind != TrackKind::Video || track.hidden) continue;
        std::vector<const Clip*> clips;
        for (const Clip* c : ev.trackClips(track.id))
            if (ev.clipVisibleAt(*c, t)) clips.push_back(c);
        std::ranges::sort(clips, [](const Clip* a, const Clip* b) {
            return a->start != b->start ? a->start < b->start : a->id < b->id;
        });
        std::vector<Id> done;
        for (const Clip* c : clips) {
            if (std::ranges::contains(done, c->id)) continue;
            const Transition* active = nullptr;
            for (const auto& tr : ev.clipTransitions(c->id))
                if (t >= tr.start && t < tr.end) active = tr.transition;
            if (!active) {
                drawClip(ev, canvas, *c, t, unit, true, gpu);
                continue;
            }
            const Clip* a = p.findClip(active->clipA);
            const Clip* b = p.findClip(active->clipB);
            if (!a || !b) continue;
            auto bufA = std::make_shared<Image>(gpu ? Image::forOverwrite(w, h) : Image(w, h));
            auto bufB = std::make_shared<Image>(gpu ? Image::forOverwrite(w, h) : Image(w, h));
            auto mixed = std::make_shared<Image>(gpu ? Image::forOverwrite(w, h) : Image());
            if (gpu) { gpu->fill(bufA, Color{0, 0, 0, 0}); gpu->fill(bufB, Color{0, 0, 0, 0}); }
            drawClip(ev, bufA, *a, t, unit, false, gpu);
            drawClip(ev, bufB, *b, t, unit, false, gpu);
            if (gpu) gpu->transition(mixed, bufA, bufB, active->type, ev.transitionMix(*active), ev.transitionParams(*active));
            else mixTransition(*mixed, *bufA, *bufB, active->type, ev.transitionMix(*active), ev.transitionParams(*active));
            // A mixed frame is not a single opaque layer, so it needs the
            // background laid down before it blends.
            if (fillBackground_) {
                fill(*canvas, background_);
                fillBackground_ = false;
            }
            if (gpu) gpu->drawLayer(canvas, mixed, Placement{w / 2.0, h / 2.0}, 1, parseBlend(ev.str(*b, "blend")));
            else drawLayer(*canvas, *mixed, Placement{w / 2.0, h / 2.0}, 1, parseBlend(ev.str(*b, "blend")));
            done.push_back(a->id);
            done.push_back(b->id);
        }
    }
    if (fillBackground_) {
        fill(*canvas, background_);
        fillBackground_ = false;
    }
    ev.refreshOutputs();
    return canvas;
}

}  // namespace mf
