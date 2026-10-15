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

std::shared_ptr<Image> renderTextBlock(Font& font, const std::string& text, float px, Color color) {
    TextBlock b = layoutTextBlock(font, text, px);
    if (b.lines.empty()) return nullptr;
    auto img = std::make_shared<Image>(b.w, b.h);
    for (size_t i = 0; i < b.lines.size(); ++i) {
        float lw = font.measure(b.lines[i], px);
        drawText(*img, font, b.lines[i], (b.w - lw) / 2, 1 + b.lineHeight * i, px, color);
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
            // Fit the media inside the frame, keeping its aspect.
            double fit = std::min(double(w) / m->width, double(h) / m->height);
            layer.displayW = m->width * fit;
            layer.displayH = m->height * fit;
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
            float px = float(ev.num(c, "size", 96) * unit);
            if (px < 1) break;
            layer.image = renderTextBlock(*font, ev.str(c, "text"), px, ev.color(c, "color", Color::gray(1)));
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
    for (const auto& e : c.videoEffects) {
        if (!e.enabled) continue;
        applyEffect(*layer.image, e.type, ev.effectParams(e, false), unit / toDisplay);
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

std::shared_ptr<Image> FrameRenderer::renderClip(Evaluator& ev, const Clip& c, double t, int padX, int padY) {
    ev.setTime(t);
    const auto& st = ev.project().settings;
    auto canvas = std::make_shared<Image>(st.width + 2 * padX, st.height + 2 * padY);
    drawClip(ev, *canvas, c, t, 1, false, padX, padY);
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
