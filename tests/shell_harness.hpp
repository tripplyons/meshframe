#pragma once

#include "fixtures.hpp"
#include "session/commands.hpp"
#include "ui/shell.hpp"

#include <SDL3/SDL.h>

#include <memory>

// A shell on a software renderer with synthetic input.
struct ShellHarness {
    int w, h;
    SDL_Surface* surface;
    SDL_Renderer* renderer;
    std::unique_ptr<mf::ui::Painter> painter;
    mf::Session session;
    std::unique_ptr<mf::ui::Shell> shell;
    mf::Commands commands{session};
    mf::ui::Input in;
    double now = 0;

    explicit ShellHarness(mf::ui::Settings settings = {}, std::string path = {}, int width = 900, int height = 600) : w(width), h(height) {
        surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        renderer = SDL_CreateSoftwareRenderer(surface);
        painter = std::make_unique<mf::ui::Painter>(renderer);
        shell = std::make_unique<mf::ui::Shell>(session, *painter, std::move(settings), std::move(path));
        shell->registerCommands(commands);
    }
    ~ShellHarness() {
        shell.reset();
        painter.reset();
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }
    void frame() {
        shell->frame(in, {0, 0, float(w), float(h)}, 1, now);
        shell->afterDraw();
        SDL_RenderPresent(renderer);
        for (int i = 0; i < 3; ++i) in.pressed[i] = in.released[i] = false;
        in.keys.clear();
        in.text.clear();
        in.wheelX = in.wheelY = 0;
        in.clicks = 0;
        now += 1.0 / 60;
    }
    void click(float x, float y) {
        in.mouse = {x, y};
        frame();
        in.down[0] = in.pressed[0] = true;
        in.clicks = 1;
        frame();
        in.down[0] = false;
        in.released[0] = true;
        frame();
    }
    nlohmann::json run(std::vector<std::string> words) { return commands.runSync(mf::Commands::requestFromWords(words, fixtures::dir().string())); }
    mf::ui::Rect paneArea() const {
        float row = shell->ui().rowHeight();
        return {0, 0, float(w), float(h) - row - 1};
    }
    SDL_Color pixel(int x, int y) const {
        SDL_Color c{};
        SDL_ReadSurfacePixel(surface, x, y, &c.r, &c.g, &c.b, &c.a);
        return c;
    }
};
