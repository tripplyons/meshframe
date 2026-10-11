#include "audio/audio_sink.hpp"
#include "session/commands.hpp"
#include "ui/shell.hpp"

#include <SDL3/SDL.h>
#include <fmt/format.h>

#include <deque>
#include <filesystem>
#include <map>
#include <mutex>

namespace {

using namespace mf;
using namespace mf::ui;

// Native dialogs may answer on another thread; results wait here for the main loop.
struct DialogRequest {
    int id = 0;
    std::vector<std::string> patterns;
    std::vector<SDL_DialogFileFilter> filters;
};

std::mutex g_dialogMu;
std::vector<std::pair<int, std::string>> g_dialogResults;
Uint32 g_wakeEvent = 0;

void wake() {
    SDL_Event e{};
    e.type = g_wakeEvent;
    SDL_PushEvent(&e);
}

void SDLCALL dialogDone(void* userdata, const char* const* files, int) {
    auto* req = static_cast<DialogRequest*>(userdata);
    {
        std::lock_guard lock(g_dialogMu);
        g_dialogResults.emplace_back(req->id, files && files[0] ? files[0] : "");
    }
    delete req;
    wake();
}

class App {
public:
    int run(int argc, char** argv);

private:
    void buildInput();
    void processEvent(const SDL_Event& e);
    void updateCursor(Cursor c);
    void updateTitle();
    void takeDialogResults();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::unique_ptr<Session> session_;
    std::unique_ptr<Commands> commands_;
    std::unique_ptr<Painter> painter_;
    std::unique_ptr<Shell> shell_;

    Input in_;
    std::deque<SDL_Event> pending_;
    bool dropText_ = false;
    float density_ = 1;
    std::map<Cursor, SDL_Cursor*> cursors_;
    Cursor cursor_ = Cursor::Arrow;
    std::string title_;
    int nextDialog_ = 1;
    std::map<int, std::function<void(std::string)>> dialogs_;
};

int App::run(int argc, char** argv) {
    std::string projectPath, settingsPath = Settings::defaultPath();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--settings" && i + 1 < argc) settingsPath = argv[++i];
        else if (a == "--help" || a == "-h") {
            fmt::print("usage: meshframe [--settings FILE] [PROJECT.mfproj]\n");
            return 0;
        } else projectPath = a;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fmt::print(stderr, "meshframe: SDL_Init failed: {}\n", SDL_GetError());
        return 1;
    }
    g_wakeEvent = SDL_RegisterEvents(1);
    window_ = SDL_CreateWindow("meshframe", 1440, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    renderer_ = window_ ? SDL_CreateRenderer(window_, nullptr) : nullptr;
    if (!renderer_) {
        fmt::print(stderr, "meshframe: cannot create a window: {}\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer_, 1);
    SDL_StartTextInput(window_);

    std::string err;
    auto sink = openSdlAudio(48000, &err);
    session_ = std::make_unique<Session>(std::move(sink));
    if (!err.empty()) session_->log(LogLine::Level::Warn, "No audio output: " + err);
    if (!projectPath.empty() && !session_->open(projectPath, &err)) session_->log(LogLine::Level::Error, err);

    std::string settingsErr;
    Settings settings = Settings::load(settingsPath, &settingsErr);
    if (!settingsErr.empty()) session_->log(LogLine::Level::Warn, settingsErr);
    painter_ = std::make_unique<Painter>(renderer_);
    shell_ = std::make_unique<Shell>(*session_, *painter_, std::move(settings), settingsPath);
    commands_ = std::make_unique<Commands>(*session_);
    shell_->registerCommands(*commands_);
    shell_->hooks.fileDialog = [this](bool save, const std::string& title, const std::vector<std::string>& exts,
                                      std::function<void(std::string)> done) {
        auto* req = new DialogRequest{nextDialog_++, {}, {}};
        dialogs_[req->id] = std::move(done);
        if (!exts.empty()) {
            std::string pattern;
            for (const auto& e : exts) pattern += (pattern.empty() ? "" : ";") + e;
            req->patterns = {title, pattern};
            req->filters.push_back({req->patterns[0].c_str(), req->patterns[1].c_str()});
        }
        auto* filters = req->filters.empty() ? nullptr : req->filters.data();
        int count = int(req->filters.size());
        if (save) SDL_ShowSaveFileDialog(dialogDone, req, window_, filters, count, nullptr);
        else SDL_ShowOpenFileDialog(dialogDone, req, window_, filters, count, nullptr, false);
    };

    while (!shell_->quitRequested()) {
        if (pending_.empty()) {
            SDL_Event e;
            if (shell_->wantsFrame()) {
                while (SDL_PollEvent(&e)) pending_.push_back(e);
            } else if (SDL_WaitEventTimeout(&e, 250)) {
                pending_.push_back(e);
                while (SDL_PollEvent(&e)) pending_.push_back(e);
            }
        } else {
            SDL_Event e;
            while (SDL_PollEvent(&e)) pending_.push_back(e);
        }
        takeDialogResults();
        buildInput();

        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        density_ = SDL_GetWindowPixelDensity(window_);
        float scale = SDL_GetWindowDisplayScale(window_);
        shell_->frame(in_, {0, 0, float(w), float(h)}, scale > 0 ? scale : 1, double(SDL_GetTicksNS()) / 1e9);
        shell_->afterDraw();
        SDL_RenderPresent(renderer_);
        updateCursor(shell_->ui().cursor());
        updateTitle();
    }

    shell_.reset();
    commands_.reset();
    painter_.reset();
    session_.reset();
    for (auto& [c, cur] : cursors_) SDL_DestroyCursor(cur);
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
    return 0;
}

// Moves queued events into this frame's input. A button event waits for the
// next frame after a mouse move or another button event, so widgets see the
// hover before the press and the press before the release.
void App::buildInput() {
    for (int i = 0; i < 3; ++i) in_.pressed[i] = in_.released[i] = false;
    in_.keys.clear();
    in_.text.clear();
    in_.wheelX = in_.wheelY = 0;
    in_.clicks = 0;
    bool moved = false, button = false;
    while (!pending_.empty()) {
        const SDL_Event& e = pending_.front();
        bool isButton = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP;
        if (isButton && (moved || button)) break;
        if (e.type == SDL_EVENT_MOUSE_MOTION) moved = true;
        button |= isButton;
        processEvent(e);
        pending_.pop_front();
    }
    in_.mods = modsFromSdl(SDL_GetModState());
}

void App::processEvent(const SDL_Event& e) {
    auto buttonIndex = [](Uint8 b) { return b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_RIGHT ? 1 : b == SDL_BUTTON_MIDDLE ? 2 : -1; };
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: shell_->requestQuit(); break;
    case SDL_EVENT_MOUSE_MOTION: in_.mouse = {e.motion.x * density_, e.motion.y * density_}; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        in_.mouse = {e.button.x * density_, e.button.y * density_};
        int b = buttonIndex(e.button.button);
        if (b < 0) break;
        in_.down[b] = in_.pressed[b] = true;
        if (b == 0) in_.clicks = e.button.clicks;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        in_.mouse = {e.button.x * density_, e.button.y * density_};
        int b = buttonIndex(e.button.button);
        if (b < 0) break;
        in_.down[b] = false;
        in_.released[b] = true;
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (!in_.down[0] && !in_.down[1]) in_.mouse = {-1, -1};
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float flip = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        in_.wheelX += e.wheel.x * flip;
        in_.wheelY += e.wheel.y * flip;
        break;
    }
    case SDL_EVENT_KEY_DOWN: {
        dropText_ = false;
        if (isModifierKey(e.key.scancode)) break;
        KeyChord c{e.key.scancode, modsFromSdl(e.key.mod)};
        // Actions run before text input, so Option+1 switches spaces even in
        // the script editor instead of typing a special character.
        if (shell_->handleKey(c)) {
            dropText_ = (c.mods & (ModAlt | ModCtrl | ModGui)) != 0 || !shell_->ui().typing();
            break;
        }
        in_.keys.push_back(c);
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (dropText_) dropText_ = false;
        else in_.text += e.text.text;
        break;
    case SDL_EVENT_DROP_FILE: {
        std::string err;
        if (e.drop.data && !session_->importMedia(e.drop.data, &err)) shell_->notify(err, true);
        break;
    }
    default: break;
    }
}

void App::updateCursor(Cursor c) {
    if (c == cursor_) return;
    cursor_ = c;
    SDL_SystemCursor sys = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (c) {
    case Cursor::Arrow: sys = SDL_SYSTEM_CURSOR_DEFAULT; break;
    case Cursor::Text: sys = SDL_SYSTEM_CURSOR_TEXT; break;
    case Cursor::ResizeH: sys = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
    case Cursor::ResizeV: sys = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
    case Cursor::Hand: sys = SDL_SYSTEM_CURSOR_POINTER; break;
    case Cursor::Move: sys = SDL_SYSTEM_CURSOR_MOVE; break;
    }
    auto& cur = cursors_[c];
    if (!cur) cur = SDL_CreateSystemCursor(sys);
    SDL_SetCursor(cur);
}

void App::updateTitle() {
    const std::string& path = session_->path();
    std::string name = path.empty() ? session_->project().name : std::filesystem::path(path).filename().string();
    std::string title = fmt::format("{}{} - meshframe", name, session_->dirty() ? " (modified)" : "");
    if (title == title_) return;
    title_ = title;
    SDL_SetWindowTitle(window_, title.c_str());
}

void App::takeDialogResults() {
    std::vector<std::pair<int, std::string>> results;
    {
        std::lock_guard lock(g_dialogMu);
        results.swap(g_dialogResults);
    }
    for (auto& [id, path] : results) {
        auto it = dialogs_.find(id);
        if (it == dialogs_.end()) continue;
        auto done = std::move(it->second);
        dialogs_.erase(it);
        done(path);
    }
}

}  // namespace

int main(int argc, char** argv) { return App().run(argc, argv); }
