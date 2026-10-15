#include "engine/frame_renderer.hpp"

#include "render/compositor.hpp"
#include "render/font.hpp"
#include "render/raster.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mf {

namespace {

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

FrameRenderer::Layer FrameRenderer::content(Evaluator& ev, const Clip& c, double t, double unit, int w, int h) {
    const Project& p = ev.project();
    Layer layer;
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
            if (frame) layer.image = std::make_shared<Image>(*frame);
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
            layer.image = std::make_shared<Image>(w, h);
            fill(*layer.image, ev.color(c, "color", Color::gray(0.5f)));
            layer.displayW = w;
            layer.displayH = h;
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
            layer.image = std::make_shared<Image>(w, h);
            drawList(*layer.image, dl, float(unit));
            layer.displayW = w;
            layer.displayH = h;
            break;
        }
        case ClipKind::Synth: break;
    }
    return layer;
}

bool FrameRenderer::drawClip(Evaluator& ev, Image& dst, const Clip& c, double t, double unit, bool useBlend, int padX, int padY) {
    Layer layer = content(ev, c, t, unit, dst.w - 2 * padX, dst.h - 2 * padY);
    if (!layer.image || layer.image->empty()) return false;
    double toDisplay = layer.displayW / layer.image->w;
    double effectUnit = unit / toDisplay;
    // Room for effects that draw past the edges, such as a drop shadow.
    int margin = 0;
    for (const auto& e : c.videoEffects)
        if (e.enabled) margin += effectMargin(e.type, ev.effectParams(e, false), effectUnit);
    PixelBox content{margin, margin, layer.image->w, layer.image->h};
    if (margin > 0) {
        auto padded = std::make_shared<Image>(layer.image->w + 2 * margin, layer.image->h + 2 * margin);
        for (int y = 0; y < layer.image->h; ++y) std::copy_n(layer.image->row(y), layer.image->w * 4, padded->at(margin, margin + y));
        layer.displayW *= double(padded->w) / layer.image->w;
        layer.displayH *= double(padded->h) / layer.image->h;
        layer.image = std::move(padded);
    }
    for (const auto& e : c.videoEffects) {
        if (!e.enabled) continue;
        applyEffect(*layer.image, e.type, ev.effectParams(e, false), effectUnit, content);
    }
    double scale = ev.num(c, "scale", 1);
    Placement at;
    at.cx = dst.w / 2.0 + ev.num(c, "x") * unit;
    at.cy = dst.h / 2.0 + ev.num(c, "y") * unit;
    at.sx = scale * layer.displayW / layer.image->w;
    at.sy = scale * layer.displayH / layer.image->h;
    at.rotation = ev.num(c, "rotation") * std::numbers::pi / 180;
    float opacity = float(std::clamp(ev.num(c, "opacity", 1), 0.0, 1.0));
    drawLayer(dst, *layer.image, at, opacity, useBlend ? parseBlend(ev.str(c, "blend")) : Blend::Normal);
    return true;
}

std::shared_ptr<Image> FrameRenderer::renderClip(Evaluator& ev, const Clip& c, double t, double scale, int padX, int padY) {
    ev.setTime(t);
    const auto& st = ev.project().settings;
    int w = std::max(2, int(std::lround(st.width * scale))), h = std::max(2, int(std::lround(st.height * scale)));
    auto canvas = std::make_shared<Image>(w + 2 * padX, h + 2 * padY);
    drawClip(ev, *canvas, c, t, double(w) / st.width, false, padX, padY);
    ev.refreshOutputs();
    return canvas;
}

std::shared_ptr<Image> FrameRenderer::render(Evaluator& ev, double t, int w, int h) {
    ev.setTime(t);
    const Project& p = ev.project();
    double unit = double(w) / p.settings.width;
    auto canvas = std::make_shared<Image>(w, h);
    fill(*canvas, p.settings.background);

    for (const auto& track : p.tracks) {
        if (track.kind != TrackKind::Video || track.hidden) continue;
        std::vector<const Clip*> clips;
        for (const auto& c : p.clips)
            if (c.track == track.id && ev.clipVisibleAt(c, t)) clips.push_back(&c);
        std::ranges::sort(clips, [](const Clip* a, const Clip* b) {
            return a->start != b->start ? a->start < b->start : a->id < b->id;
        });
        std::vector<Id> done;
        for (const Clip* c : clips) {
            if (std::ranges::contains(done, c->id)) continue;
            const Transition* active = nullptr;
            for (const auto& tr : p.transitions) {
                if (tr.clipA != c->id && tr.clipB != c->id) continue;
                auto [ws, we] = Evaluator::window(p, tr);
                if (t >= ws && t < we) active = &tr;
            }
            if (!active) {
                drawClip(ev, *canvas, *c, t, unit, true);
                continue;
            }
            const Clip* a = p.findClip(active->clipA);
            const Clip* b = p.findClip(active->clipB);
            if (!a || !b) continue;
            Image bufA(w, h), bufB(w, h), mixed;
            drawClip(ev, bufA, *a, t, unit, false);
            drawClip(ev, bufB, *b, t, unit, false);
            mixTransition(mixed, bufA, bufB, active->type, ev.transitionMix(*active), ev.transitionParams(*active));
            drawLayer(*canvas, mixed, Placement{w / 2.0, h / 2.0}, 1, parseBlend(ev.str(*b, "blend")));
            done.push_back(a->id);
            done.push_back(b->id);
        }
    }
    ev.refreshOutputs();
    return canvas;
}

}  // namespace mf
