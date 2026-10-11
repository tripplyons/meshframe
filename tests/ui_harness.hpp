#pragma once

#include "render/font.hpp"
#include "ui/context.hpp"
#include "ui/painter.hpp"
#include "ui/theme.hpp"

#include <SDL3/SDL.h>

#include <memory>

// Drives the immediate-mode UI with synthetic input on an SDL software renderer.
struct UiHarness {
    SDL_Surface* surface = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<mf::ui::Painter> painter;
    mf::ui::Theme theme;
    std::unique_ptr<mf::ui::Context> ui;
    mf::ui::Input in;
    double now = 0;
    int w, h;

    UiHarness(int width = 400, int height = 300) : w(width), h(height) {
        surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        renderer = SDL_CreateSoftwareRenderer(surface);
        painter = std::make_unique<mf::ui::Painter>(renderer);
        painter->setFont(mf::ui::FontRole::Ui, mf::Font::sans(), 13);
        painter->setFont(mf::ui::FontRole::Mono, mf::Font::mono(), 13);
        ui = std::make_unique<mf::ui::Context>(*painter, theme);
    }
    ~UiHarness() {
        ui.reset();
        painter.reset();
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }

    // Runs one frame with the current input, then clears per-frame input.
    template <class F>
    void frame(F&& draw) {
        painter->begin(w, h);
        ui->beginFrame(in, {0, 0, float(w), float(h)}, 1, now);
        draw(*ui);
        ui->endFrame();
        SDL_RenderPresent(renderer);
        for (int i = 0; i < 3; ++i) in.pressed[i] = in.released[i] = false;
        in.keys.clear();
        in.text.clear();
        in.wheelX = in.wheelY = 0;
        in.clicks = 0;
        now += 1.0 / 60;
    }
    void move(float x, float y) { in.mouse = {x, y}; }
    void press(int b = 0) {
        in.down[b] = in.pressed[b] = true;
        if (b == 0) in.clicks = 1;
    }
    void release(int b = 0) {
        in.down[b] = false;
        in.released[b] = true;
    }
    void key(SDL_Scancode sc, uint8_t mods = 0) { in.keys.push_back({sc, mods}); }

    // Reads a pixel from the rendered surface.
    SDL_Color pixel(int x, int y) const {
        SDL_Color c{};
        SDL_ReadSurfacePixel(surface, x, y, &c.r, &c.g, &c.b, &c.a);
        return c;
    }
};
