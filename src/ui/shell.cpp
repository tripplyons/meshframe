#include "ui/shell.hpp"

#include "model/edit.hpp"
#include "render/image.hpp"
#include "ui/widgets.hpp"

#include <fmt/format.h>

#include <filesystem>

namespace mf::ui {

namespace {

constexpr double kNoticeSeconds = 4;
constexpr double kSettingsSaveDelay = 1;

SDL_Scancode digitKey(int n) { return n == 10 ? SDL_SCANCODE_0 : SDL_Scancode(SDL_SCANCODE_1 + n - 1); }

// A view for an unknown type name, such as one from a newer settings file.
class MissingView : public View {
public:
    explicit MissingView(std::string name) : name_(std::move(name)) {}
    void draw(ViewEnv& env, const Rect& body) override {
        env.ui.painter.textIn(body, fmt::format("Unknown view \"{}\"", name_), env.ui.col(Slot::TextDim), Align::Center);
    }

private:
    std::string name_;
};

std::vector<std::string> viewLabels() {
    std::vector<std::string> out;
    for (const auto& v : viewTypes()) out.push_back(v.label);
    return out;
}

std::vector<std::string> layoutLabels() {
    std::vector<std::string> out;
    for (const auto& l : layoutPresets()) out.push_back(l.label);
    return out;
}

ArgSpec req(std::string name, std::string type, std::string help = {}) { return {std::move(name), std::move(type), true, std::move(help)}; }
ArgSpec opt(std::string name, std::string type, std::string help = {}) { return {std::move(name), std::move(type), false, std::move(help)}; }

int spaceArg(const json& args, const char* name, int current) {
    if (!args.contains(name) || args[name].is_null()) return current;
    int n = args[name].get<int>();
    if (n < 1 || n > Workspace::kSpaces) throw EditError(fmt::format("space must be 1 to {}", Workspace::kSpaces));
    return n - 1;
}

}  // namespace

Shell::Shell(Session& session, Painter& painter, Settings settings, std::string settingsPath)
    : session_(session),
      painter_(painter),
      settings_(std::move(settings)),
      settingsPath_(std::move(settingsPath)),
      ui_(painter, settings_.theme) {
    registerActions();
    actions_.loadOverrides(settings_.bindings);
    workspace_.loadJson(settings_.workspace);
}

Shell::~Shell() {
    if (settingsDirty_) saveSettings();
}

// ---- settings ------------------------------------------------------------------------

void Shell::applyFonts(float scale) {
    auto load = [&](const std::string& path, bool mono) {
        if (!path.empty()) {
            std::string err;
            if (auto f = Font::load(path, &err)) return f;
            notify(fmt::format("Cannot load font {}: {}", path, err), true);
        }
        return mono ? Font::mono() : Font::sans();
    };
    painter_.setFont(FontRole::Ui, load(settings_.uiFont, false), std::round(settings_.uiSize * scale));
    painter_.setFont(FontRole::Mono, load(settings_.monoFont, true), std::round(settings_.monoSize * scale));
    fontScale_ = scale;
    fontsDirty_ = false;
}

void Shell::settingsChanged() {
    actions_.loadOverrides(settings_.bindings);
    fontsDirty_ = true;
    settingsDirty_ = true;
    settingsDirtyAt_ = ui_.now();
}

void Shell::workspaceChanged() {
    settingsDirty_ = true;
    settingsDirtyAt_ = ui_.now();
}

void Shell::saveSettings() {
    settingsDirty_ = false;
    settings_.workspace = workspace_.toJson();
    settings_.bindings = actions_.overridesJson();
    if (settingsPath_.empty()) return;
    std::string err;
    if (!settings_.save(settingsPath_, &err)) notify("Cannot save settings: " + err, true);
}

// ---- messages and edits ----------------------------------------------------------

void Shell::notify(std::string text, bool error) {
    if (error) session_.log(LogLine::Level::Error, text);
    notice_ = std::move(text);
    noticeError_ = error;
    noticeAt_ = ui_.now();
    ui_.requestFrame();
}

bool Shell::edit(const std::string& label, const std::function<void(Project&)>& fn) {
    try {
        session_.apply(label, fn);
        return true;
    } catch (const std::exception& e) {
        notify(e.what(), true);
        return false;
    }
}

void Shell::requestQuit() {
    if (session_.dirty() && ui_.now() - quitAskedAt_ > 3) {
        quitAskedAt_ = ui_.now();
        notify("The project has unsaved changes. Quit again to discard them.");
        return;
    }
    quit_ = true;
}

// ---- files -------------------------------------------------------------------------

void Shell::newProject() {
    session_.newProject();
    notify("New project");
}

void Shell::openProject() {
    if (!hooks.fileDialog) return notify("No file dialog available; use the CLI open command", true);
    hooks.fileDialog(false, "Open project", {"mfproj"}, [this](std::string path) {
        if (path.empty()) return;
        std::string err;
        if (session_.open(path, &err)) notify("Opened " + path);
        else notify(err, true);
    });
}

void Shell::saveProject(bool saveAs) {
    auto write = [this](const std::string& path) {
        std::string err;
        if (session_.save(path, &err)) notify("Saved " + path);
        else notify(err, true);
    };
    if (!saveAs && !session_.path().empty()) return write(session_.path());
    if (!hooks.fileDialog) return notify("No file dialog available; use the CLI save command", true);
    hooks.fileDialog(true, "Save project", {"mfproj"}, [write](std::string path) {
        if (path.empty()) return;
        if (std::filesystem::path(path).extension() != ".mfproj") path += ".mfproj";
        write(path);
    });
}

void Shell::importMedia() {
    if (!hooks.fileDialog) return notify("No file dialog available; use the CLI media.import command", true);
    hooks.fileDialog(false, "Import media", {}, [this](std::string path) {
        if (path.empty()) return;
        std::string err;
        if (Id id = session_.importMedia(path, &err)) {
            session_.selection.media = id;
            notify("Imported " + std::filesystem::path(path).filename().string());
        } else {
            notify(err, true);
        }
    });
}

// ---- views -------------------------------------------------------------------------

View* Shell::paneView(int pane) {
    Space& s = workspace_.active();
    if (pane < 0 || pane >= int(s.panes.size())) return nullptr;
    Pane& p = s.panes[pane];
    if (auto* v = dynamic_cast<View*>(p.state.get())) return v;
    const ViewType* type = findViewType(p.view);
    p.state = type ? type->make() : std::make_unique<MissingView>(p.view);
    return static_cast<View*>(p.state.get());
}

bool Shell::routeToView(std::string_view type, std::string_view action) {
    Space& s = workspace_.active();
    int count = findLayout(s.layout) ? findLayout(s.layout)->panes() : 0;
    int target = -1;
    if (focusedPane_ < count && s.panes[focusedPane_].view == type) target = focusedPane_;
    for (int i = 0; i < count && target < 0; ++i)
        if (s.panes[i].view == type) target = i;
    if (target < 0) return false;
    ViewEnv env{ui_, session_, *this, target == focusedPane_};
    return paneView(target)->command(env, action);
}

void Shell::showView(const std::string& type) {
    Space& s = workspace_.active();
    int count = findLayout(s.layout)->panes();
    for (int i = 0; i < count; ++i)
        if (s.panes[i].view == type) {
            focusedPane_ = i;
            return;
        }
    workspace_.setPaneView(workspace_.current(), std::min(focusedPane_, count - 1), type);
    workspaceChanged();
}

// ---- frame -------------------------------------------------------------------------

void Shell::frame(const Input& in, Rect screen, float displayScale, double now) {
    float scale = displayScale * settings_.uiScale;
    if (fontsDirty_ || scale != fontScale_) applyFonts(scale);
    screen_ = screen;
    painter_.begin(int(screen.w), int(screen.h));
    ui_.beginFrame(in, screen, scale, now);
    session_.tick();
    if (capture_ && (in.pressed[0] || in.pressed[1] || in.pressed[2])) capture_.reset();

    painter_.fill(screen, ui_.col(Slot::Background));
    Rect area = screen;
    Rect status = cutBottom(area, ui_.rowHeight());
    painter_.fill(cutBottom(area, 1), ui_.col(Slot::Border));
    drawPanes(area);
    drawStatus(status);
    ui_.endFrame();

    if (settingsDirty_ && now - settingsDirtyAt_ > kSettingsSaveDelay) saveSettings();
}

void Shell::drawPanes(const Rect& area) {
    int spaceIndex = workspace_.current();
    Space& s = workspace_.active();
    LayoutRects lr = Workspace::compute(s, area, ui_.rowHeight() * 2 + 1);
    if (focusedPane_ >= int(lr.panes.size())) focusedPane_ = 0;
    static const std::vector<std::string> labels = viewLabels();

    for (int i = 0; i < int(lr.panes.size()); ++i) {
        Rect r = lr.panes[i];
        if (ui_.mouseIn(r) && (ui_.input().pressed[0] || ui_.input().pressed[1])) focusedPane_ = i;
        View* view = paneView(i);
        // Ids follow the view object so widget state moves with a swapped pane.
        ui_.pushId(uint64_t(reinterpret_cast<uintptr_t>(view)));
        ViewEnv env{ui_, session_, *this, i == focusedPane_};

        Rect header = cutTop(r, ui_.rowHeight());
        painter_.fill(cutTop(r, 1), ui_.col(Slot::Border));
        painter_.fill(header, ui_.col(Slot::Panel));
        int index = 0;
        for (size_t k = 0; k < viewTypes().size(); ++k)
            if (viewTypes()[k].id == s.panes[i].view) index = int(k);
        Rect selector = cutLeft(header, std::min(header.w, ui_.dp(130)));
        if (dropdown(ui_, selector, "##view", index, labels)) {
            workspace_.setPaneView(spaceIndex, i, viewTypes()[index].id);
            workspaceChanged();
            view = paneView(i);
        }
        if (i == focusedPane_) painter_.fill({selector.x, selector.b() - ui_.dp(2), selector.w, ui_.dp(2)}, ui_.col(Slot::Accent));
        painter_.pushClip(header);
        view->header(env, header);
        painter_.popClip();

        painter_.pushClip(r);
        view->draw(env, r);
        painter_.popClip();
        ui_.popId();
    }

    // Dividers come last so their grab zones win over pane content near the edge.
    const LayoutPreset* preset = findLayout(s.layout);
    for (const Divider& d : lr.dividers) {
        painter_.fill(d.rect, ui_.col(Slot::Border));
        float grab = ui_.dp(3);
        Rect zone = d.vertical ? Rect{d.rect.x - grab, d.rect.y, d.rect.w + 2 * grab, d.rect.h}
                               : Rect{d.rect.x, d.rect.y - grab, d.rect.w, d.rect.h + 2 * grab};
        Hit h = ui_.interact(ui_.id(fmt::format("divider{}", d.index)), zone);
        if (h.hovered || h.held) ui_.setCursor(d.vertical ? Cursor::ResizeH : Cursor::ResizeV);
        if (h.doubleClicked && preset) {
            s.dividers[d.index] = defaultDividers(preset->root)[d.index];
            workspaceChanged();
        } else if (h.held && !h.pressed) {
            float pos = d.vertical ? ui_.mouse().x - d.split.x : ui_.mouse().y - d.split.y;
            float extent = d.vertical ? d.split.w : d.split.h;
            if (extent > 0) {
                s.dividers[d.index] = std::clamp(pos / extent, d.min, std::max(d.min, d.max));
                workspaceChanged();
            }
        }
    }
}

void Shell::drawStatus(const Rect& bar) {
    painter_.fill(bar, ui_.col(Slot::Panel));
    Rect r = bar;
    float cell = ui_.dp(24);
    for (int i = 0; i < Workspace::kSpaces; ++i) {
        Rect b = cutLeft(r, cell);
        if (button(ui_, b, fmt::format("{}##space", i + 1), i == workspace_.current())) {
            workspace_.switchTo(i);
            workspaceChanged();
        }
    }
    painter_.fill(cutLeft(r, 1), ui_.col(Slot::Border));
    static const std::vector<std::string> layouts = layoutLabels();
    int layout = 0;
    for (size_t k = 0; k < layoutPresets().size(); ++k)
        if (layoutPresets()[k].name == workspace_.active().layout) layout = int(k);
    if (dropdown(ui_, cutLeft(r, std::min(r.w, ui_.dp(130))), "##layout", layout, layouts)) {
        workspace_.setLayout(workspace_.current(), layoutPresets()[layout].name);
        workspaceChanged();
    }
    painter_.fill(cutLeft(r, 1), ui_.col(Slot::Border));
    WidgetId menu = ui_.id("##file-menu");
    Rect fileButton = cutLeft(r, std::min(r.w, ui_.dp(56)));
    if (button(ui_, fileButton, "Menu##file")) ui_.openPopup(menu, {fileButton.x, fileButton.y});
    drawFileMenu(menu);
    painter_.fill(cutLeft(r, 1), ui_.col(Slot::Border));

    // Right side: transport time, then export progress or the project name.
    const Project& p = session_.project();
    Rect inner = r.inset(ui_.pad(), 0);
    std::string time = formatTimecode(session_.time(), p.settings.fps);
    if (session_.player().playing()) time = "Playing  " + time;
    float tw = painter_.measure(time);
    painter_.textIn(cutRight(inner, tw), time, ui_.col(Slot::Text), Align::Right);
    cutRight(inner, ui_.dp(16));
    std::string right;
    ExportJob& job = session_.exportJob();
    if (job.running()) right = fmt::format("Exporting {:.0f}%", job.progress() * 100);
    else right = (session_.path().empty() ? p.name : std::filesystem::path(session_.path()).filename().string()) + (session_.dirty() ? " (modified)" : "");
    float rw = std::min(inner.w / 2, painter_.measure(right));
    painter_.textIn(cutRight(inner, rw), right, ui_.col(Slot::TextDim), Align::Right);
    cutRight(inner, ui_.dp(16));
    if (!notice_.empty() && ui_.now() - noticeAt_ < kNoticeSeconds)
        painter_.textIn(inner, notice_, noticeError_ ? ui_.col(Slot::Error) : ui_.col(Slot::Text));
}

void Shell::drawFileMenu(WidgetId id) {
    // Opens upward from the status bar once its height is known.
    float barTop = screen_.b() - ui_.rowHeight();
    if (ui_.popupOpen(id) && ui_.popupHeight(id) > 0) ui_.openPopup(id, {ui_.popupPos().x, barTop - ui_.popupHeight(id)});
    if (!beginMenu(ui_, id, ui_.dp(240))) return;
    auto shortcut = [&](const char* action) {
        const auto& b = actions_.bindings(action);
        return b.empty() ? std::string() : formatChord(b.front());
    };
    struct Item {
        const char* label;
        const char* action;
    };
    const Item items[] = {
        {"New project", "file.new"},   {"Open project...", "file.open"}, {"Save", "file.save"},
        {"Save as...", "file.save-as"}, {"Import media...", "file.import"}, {nullptr, nullptr},
        {"Undo", "edit.undo"},          {"Redo", "edit.redo"},             {nullptr, nullptr},
        {"Settings", "app.settings"},   {"Quit", "app.quit"},
    };
    for (const auto& it : items) {
        if (!it.label) {
            menuSeparator(ui_);
            continue;
        }
        if (menuItem(ui_, it.label, shortcut(it.action))) actions_.run(it.action);
    }
    endMenu(ui_);
}

bool Shell::handleKey(const KeyChord& c) {
    if (capture_) {
        BindingCapture cap = std::move(*capture_);
        capture_.reset();
        ui_.requestFrame();
        if (c.key != SDL_SCANCODE_ESCAPE || c.mods != 0) bindChord(cap.action, c, cap.index);
        return true;
    }
    return actions_.dispatch(c, ui_.typing());
}

void Shell::captureBinding(std::string action, int index) {
    if (!actions_.find(action)) return;
    ui_.blur();  // the captured key must not also type into a field
    capture_ = BindingCapture{std::move(action), index};
}

void Shell::bindChord(const std::string& action, const KeyChord& c, int index) {
    const Action* target = actions_.find(action);
    if (!target || !c.valid()) return;
    std::vector<std::string> taken;
    for (const Action& other : actions_.all()) {
        if (other.id == action) continue;
        std::vector<KeyChord> chords = actions_.bindings(other.id);
        if (std::erase(chords, c) == 0) continue;
        actions_.setBindings(other.id, std::move(chords));
        taken.push_back(other.label);
    }
    std::vector<KeyChord> chords = actions_.bindings(action);
    if (index >= 0 && size_t(index) < chords.size()) chords[size_t(index)] = c;
    else chords.push_back(c);
    // Keep the first copy if the chord was already bound to this action.
    auto first = std::find(chords.begin(), chords.end(), c);
    chords.erase(std::remove(first + 1, chords.end(), c), chords.end());
    actions_.setBindings(action, std::move(chords));
    settings_.bindings = actions_.overridesJson();
    settingsChanged();
    std::string text = fmt::format("Bound {} to {}", formatChord(c), target->label);
    if (!taken.empty()) {
        std::string names;
        for (const auto& t : taken) names += (names.empty() ? "" : ", ") + t;
        text += fmt::format(" (removed from {})", names);
    }
    notify(text);
}

bool Shell::wantsFrame() const {
    return ui_.wantsFrame() || session_.player().playing() || session_.exportJob().running() || !screenshots_.empty() ||
           (!notice_.empty() && ui_.now() - noticeAt_ < kNoticeSeconds + 0.1);
}

// ---- screenshots ---------------------------------------------------------------------

void Shell::afterDraw() {
    if (screenshots_.empty()) return;
    auto shots = std::move(screenshots_);
    screenshots_.clear();
    SDL_Surface* raw = SDL_RenderReadPixels(painter_.renderer(), nullptr);
    SDL_Surface* rgba = raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32) : nullptr;
    if (raw) SDL_DestroySurface(raw);
    Image img;
    if (rgba) {
        img.resize(rgba->w, rgba->h);
        for (int y = 0; y < rgba->h; ++y) {
            const uint8_t* src = static_cast<const uint8_t*>(rgba->pixels) + size_t(y) * rgba->pitch;
            uint8_t* dst = img.row(y);
            std::copy(src, src + size_t(rgba->w) * 4, dst);
            for (int x = 0; x < rgba->w; ++x) dst[x * 4 + 3] = 255;
        }
        SDL_DestroySurface(rgba);
    }
    for (auto& s : shots) {
        if (img.w == 0) s.reply({{"error", std::string("cannot read the window: ") + SDL_GetError()}});
        else if (!savePng(img, s.path.c_str())) s.reply({{"error", "cannot write " + s.path}});
        else s.reply({{"path", s.path}, {"width", img.w}, {"height", img.h}});
    }
}

// ---- actions ------------------------------------------------------------------------

void Shell::registerActions() {
    auto add = [&](std::string id, std::string label, std::string group, std::vector<KeyChord> keys, std::function<void()> fn,
                   bool whileTyping = false) {
        actions_.add({std::move(id), std::move(label), std::move(group), std::move(keys), std::move(fn), whileTyping});
    };
    for (int n = 1; n <= Workspace::kSpaces; ++n) {
        add(fmt::format("space.{}", n), fmt::format("Switch to space {}", n), "Spaces", {{digitKey(n), ModAlt}},
            [this, n] {
                workspace_.switchTo(n - 1);
                workspaceChanged();
            },
            true);
    }
    for (int n = 1; n <= Workspace::kSpaces; ++n) {
        add(fmt::format("space.swap.{}", n), fmt::format("Swap with space {} and follow", n), "Spaces",
            {{digitKey(n), ModAlt | ModShift}},
            [this, n] {
                workspace_.swapWith(n - 1);
                workspaceChanged();
            },
            true);
    }

    add("transport.toggle", "Play or pause", "Transport", {{SDL_SCANCODE_SPACE, 0}}, [this] { session_.togglePlay(); });
    add("transport.start", "Go to start", "Transport", {{SDL_SCANCODE_HOME, 0}}, [this] { session_.seek(0); });
    add("transport.end", "Go to end", "Transport", {{SDL_SCANCODE_END, 0}}, [this] { session_.seek(session_.project().duration()); });
    auto step = [this](double frames, double seconds) {
        double fps = session_.project().settings.fps;
        double t = session_.time() + frames / fps + seconds;
        session_.seek(std::max(0.0, std::round(t * fps) / fps));
    };
    add("transport.prev-frame", "Previous frame", "Transport", {{SDL_SCANCODE_LEFT, 0}}, [step] { step(-1, 0); });
    add("transport.next-frame", "Next frame", "Transport", {{SDL_SCANCODE_RIGHT, 0}}, [step] { step(1, 0); });
    add("transport.back", "Back one second", "Transport", {{SDL_SCANCODE_LEFT, ModShift}}, [step] { step(0, -1); });
    add("transport.forward", "Forward one second", "Transport", {{SDL_SCANCODE_RIGHT, ModShift}}, [step] { step(0, 1); });
    add("transport.loop", "Toggle loop", "Transport", {{SDL_SCANCODE_L, ModGui}}, [this] {
        session_.player().setLoop(!session_.player().loop());
        notify(session_.player().loop() ? "Loop on" : "Loop off");
    });

    add("edit.undo", "Undo", "Edit", {{SDL_SCANCODE_Z, ModGui}}, [this] {
        if (!session_.undo()) notify("Nothing to undo");
    });
    add("edit.redo", "Redo", "Edit", {{SDL_SCANCODE_Z, ModGui | ModShift}}, [this] {
        if (!session_.redo()) notify("Nothing to redo");
    });
    add("edit.delete", "Delete selection", "Edit", {{SDL_SCANCODE_BACKSPACE, 0}, {SDL_SCANCODE_DELETE, 0}}, [this] {
        Selection& sel = session_.selection;
        if (!sel.clips.empty()) {
            std::vector<Id> ids = sel.clips;
            edit("Delete clips", [&](Project& p) {
                for (Id id : ids)
                    if (p.findClip(id)) edit::removeClip(p, id);
            });
        } else if (sel.transition) {
            Id id = sel.transition;
            edit("Delete transition", [&](Project& p) { edit::removeTransition(p, id); });
        } else if (sel.effect) {
            Id id = sel.effect;
            edit("Delete effect", [&](Project& p) { edit::removeEffect(p, id); });
        }
        session_.pruneSelection();
    });
    add("edit.split", "Split at playhead", "Edit", {{SDL_SCANCODE_B, ModGui}}, [this] {
        double t = session_.time();
        std::vector<Id> targets;
        for (const Clip& c : session_.project().clips)
            if (c.start < t && c.end() > t && (session_.selection.clips.empty() || session_.selection.hasClip(c.id)))
                targets.push_back(c.id);
        if (targets.empty()) return notify("No clip under the playhead");
        edit("Split", [&](Project& p) {
            for (Id id : targets)
                if (const Clip* c = p.findClip(id); c && c->start < t && c->end() > t) edit::splitClip(p, id, t);
        });
    });
    add("edit.select-all", "Select all clips", "Edit", {{SDL_SCANCODE_A, ModGui}}, [this] {
        session_.selection.clips.clear();
        for (const Clip& c : session_.project().clips) session_.selection.clips.push_back(c.id);
    });
    add("edit.deselect", "Clear selection", "Edit", {{SDL_SCANCODE_ESCAPE, 0}}, [this] { session_.selection = {}; });

    add("file.new", "New project", "File", {{SDL_SCANCODE_N, ModGui}}, [this] { newProject(); });
    add("file.open", "Open project", "File", {{SDL_SCANCODE_O, ModGui}}, [this] { openProject(); });
    add("file.save", "Save project", "File", {{SDL_SCANCODE_S, ModGui}}, [this] { saveProject(false); }, true);
    add("file.save-as", "Save project as", "File", {{SDL_SCANCODE_S, ModGui | ModShift}}, [this] { saveProject(true); }, true);
    add("file.import", "Import media", "File", {{SDL_SCANCODE_I, ModGui}}, [this] { importMedia(); });
    add("file.export", "Show export", "File", {{SDL_SCANCODE_E, ModGui}}, [this] { showView("export"); });

    add("timeline.zoom-in", "Zoom timeline in", "Timeline", {{SDL_SCANCODE_EQUALS, ModGui}}, [this] { routeToView("timeline", "zoom-in"); });
    add("timeline.zoom-out", "Zoom timeline out", "Timeline", {{SDL_SCANCODE_MINUS, ModGui}}, [this] { routeToView("timeline", "zoom-out"); });
    add("timeline.zoom-fit", "Fit timeline", "Timeline", {{SDL_SCANCODE_0, ModGui}}, [this] { routeToView("timeline", "zoom-fit"); });
    add("script.apply", "Apply script", "Script", {{SDL_SCANCODE_RETURN, ModGui}}, [this] { routeToView("script", "apply"); }, true);

    add("app.settings", "Show settings", "App", {{SDL_SCANCODE_COMMA, ModGui}}, [this] { showView("settings"); }, true);
    add("app.quit", "Quit", "App", {{SDL_SCANCODE_Q, ModGui}}, [this] { requestQuit(); }, true);
}

// ---- commands ------------------------------------------------------------------------

nlohmann::json Shell::state() const {
    auto spaces = workspace_.toJson()["spaces"];
    for (int i = 0; i < Workspace::kSpaces; ++i) {
        const LayoutPreset* p = findLayout(workspace_.space(i).layout);
        auto& panes = spaces[i]["panes"];
        if (p && int(panes.size()) > p->panes()) panes.erase(panes.begin() + p->panes(), panes.end());
    }
    return {{"space", workspace_.current() + 1},
            {"focusedPane", focusedPane_ + 1},
            {"spaces", spaces},
            {"size", {int(screen_.w), int(screen_.h)}},
            {"scale", ui_.scale()},
            {"typing", ui_.typing()}};
}

void Shell::registerCommands(Commands& commands) {
    commands_ = &commands;
    using A = const json&;
    using C = CommandContext&;
    commands.add({"ui.state", "Show spaces, layouts, pane views, and the window size", {}, [this](A, C) -> json { return state(); }});
    commands.add({"ui.space", "Switch to a space (1-10)", {req("space", "int")}, [this](A a, C) -> json {
                      workspace_.switchTo(spaceArg(a, "space", 0));
                      workspaceChanged();
                      return state();
                  }});
    commands.add({"ui.swap", "Swap the current space with another and follow it", {req("space", "int")}, [this](A a, C) -> json {
                      workspace_.swapWith(spaceArg(a, "space", 0));
                      workspaceChanged();
                      return state();
                  }});
    commands.add({"ui.layout", "Set a space's layout preset", {req("layout", "string"), opt("space", "int", "default: current")},
                  [this](A a, C) -> json {
                      std::string name = a["layout"];
                      if (!findLayout(name)) {
                          std::string names;
                          for (const auto& l : layoutPresets()) names += (names.empty() ? "" : ", ") + l.name;
                          throw EditError(fmt::format("unknown layout \"{}\" (one of {})", name, names));
                      }
                      workspace_.setLayout(spaceArg(a, "space", workspace_.current()), name);
                      workspaceChanged();
                      return state();
                  }});
    commands.add({"ui.pane", "Set the view of a pane (1-based, reading order)",
                  {req("pane", "int"), req("view", "string"), opt("space", "int", "default: current")}, [this](A a, C) -> json {
                      std::string view = a["view"];
                      if (!findViewType(view)) {
                          std::string names;
                          for (const auto& v : viewTypes()) names += (names.empty() ? "" : ", ") + v.id;
                          throw EditError(fmt::format("unknown view \"{}\" (one of {})", view, names));
                      }
                      int space = spaceArg(a, "space", workspace_.current());
                      int pane = a["pane"].get<int>() - 1;
                      const LayoutPreset* p = findLayout(workspace_.space(space).layout);
                      if (pane < 0 || pane >= p->panes())
                          throw EditError(fmt::format("pane must be 1 to {} in layout {}", p->panes(), p->name));
                      workspace_.setPaneView(space, pane, view);
                      workspaceChanged();
                      return state();
                  }});
    commands.add({"ui.actions", "List actions with their key bindings", {}, [this](A, C) -> json {
                      json out = json::array();
                      for (const auto& act : actions_.all()) {
                          json keys = json::array();
                          for (const auto& k : actions_.bindings(act.id)) keys.push_back(formatChord(k));
                          out.push_back({{"id", act.id}, {"label", act.label}, {"group", act.group}, {"keys", keys}});
                      }
                      return out;
                  }});
    commands.add({"ui.action", "Run an action by id, as if its key was pressed", {req("id", "string")}, [this](A a, C) -> json {
                      std::string id = a["id"];
                      if (!actions_.run(id)) throw EditError(fmt::format("unknown action \"{}\" (see ui.actions)", id));
                      return state();
                  }});
    commands.add({"ui.bind", "Set an action's keys, comma separated (\"Alt+1, Ctrl+1\"); no keys resets to defaults",
                  {req("id", "string"), opt("keys", "string")}, [this](A a, C) -> json {
                      std::string id = a["id"];
                      if (!actions_.find(id)) throw EditError(fmt::format("unknown action \"{}\" (see ui.actions)", id));
                      std::string keys = a.value("keys", "");
                      if (keys.empty()) {
                          actions_.resetBindings(id);
                      } else {
                          std::vector<KeyChord> chords;
                          size_t start = 0;
                          while (start <= keys.size()) {
                              size_t end = std::min(keys.find(',', start), keys.size());
                              std::string part = keys.substr(start, end - start);
                              std::erase(part, ' ');
                              if (!part.empty()) {
                                  auto c = parseChord(part);
                                  if (!c) throw EditError(fmt::format("cannot parse key \"{}\"", part));
                                  chords.push_back(*c);
                              }
                              start = end + 1;
                          }
                          actions_.setBindings(id, chords);
                      }
                      settings_.bindings = actions_.overridesJson();
                      settingsChanged();
                      json out = json::array();
                      for (const auto& k : actions_.bindings(id)) out.push_back(formatChord(k));
                      return out;
                  }});
    commands.add({"ui.theme", "Show theme colors, or set one (\"accent #808080\"); color \"default\" resets it",
                  {opt("slot", "string"), opt("color", "string")}, [this](A a, C) -> json {
                      if (!a.contains("slot") || a["slot"].is_null()) {
                          json out = json::object();
                          for (int i = 0; i < kSlotCount; ++i) out[slotName(Slot(i))] = formatColor(settings_.theme.colors[i]);
                          return out;
                      }
                      std::string name = a["slot"];
                      auto slot = parseSlot(name);
                      if (!slot) throw EditError(fmt::format("unknown color slot \"{}\" (see ui.theme)", name));
                      if (a.contains("color") && !a["color"].is_null()) {
                          std::string text = a["color"];
                          if (text == "default") {
                              settings_.theme[*slot] = defaultColor(*slot);
                          } else {
                              auto c = parseColor(text);
                              if (!c) throw EditError(fmt::format("cannot parse color \"{}\"", text));
                              settings_.theme[*slot] = *c;
                          }
                          settingsChanged();
                      }
                      return formatColor(settings_.theme[*slot]);
                  }});
    commands.add({"ui.settings", "Show settings, or merge a JSON object into them", {opt("set", "json")}, [this](A a, C) -> json {
                      settings_.workspace = workspace_.toJson();
                      settings_.bindings = actions_.overridesJson();
                      if (a.contains("set") && !a["set"].is_null()) {
                          if (!a["set"].is_object()) throw EditError("set must be a JSON object");
                          json merged = settings_.toJson();
                          merged.merge_patch(a["set"]);
                          Settings next = Settings::fromJson(merged);
                          settings_.theme = next.theme;
                          settings_.bindings = next.bindings;
                          settings_.uiFont = next.uiFont;
                          settings_.monoFont = next.monoFont;
                          settings_.uiSize = next.uiSize;
                          settings_.monoSize = next.monoSize;
                          settings_.uiScale = next.uiScale;
                          if (a["set"].contains("workspace")) workspace_.loadJson(next.workspace);
                          settingsChanged();
                      }
                      return settings_.toJson();
                  }});
    commands.add({"ui.screenshot", "Save the window as a PNG after the next frame", {req("path", "path")}, [this](A a, C c) -> json {
                      std::string path = a["path"];
                      if (std::filesystem::path(path).extension() != ".png") throw EditError("screenshot path must end in .png");
                      screenshots_.push_back({path, c.defer()});
                      ui_.requestFrame();
                      return nullptr;
                  }});
}

}  // namespace mf::ui
