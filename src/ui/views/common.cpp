#include "ui/views/common.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>

namespace mf::ui::views {

const Property* findProperty(const Project& cp, Id target, const std::string& name) {
    Project& p = const_cast<Project&>(cp);
    const PropertyMap* props = nullptr;
    if (const Clip* c = p.findClip(target)) props = &c->props;
    else if (const Effect* e = p.findEffect(target)) props = &e->props;
    else if (const Transition* t = p.findTransition(target)) props = &t->props;
    if (!props) return nullptr;
    auto it = props->find(name);
    return it == props->end() ? nullptr : &it->second;
}

double targetStart(const Project& cp, Id target) {
    Project& p = const_cast<Project&>(cp);
    if (const Clip* c = p.findClip(target)) return c->start;
    Clip* owner = nullptr;
    if (p.findEffect(target, &owner)) return owner ? owner->start : 0;
    if (const Transition* t = p.findTransition(target)) return Evaluator::window(p, *t).first;
    return 0;
}

Value evaluatedValue(Evaluator& ev, Id target, const std::string& name) {
    Project& p = const_cast<Project&>(ev.project());
    if (const Clip* c = p.findClip(target)) return ev.prop(*c, name);
    if (const Effect* e = p.findEffect(target)) return ev.effectProp(*e, name);
    if (const Transition* t = p.findTransition(target)) return ev.transitionProp(*t, name);
    return {};
}

std::string propertyError(const Evaluator& ev, Id target, const std::string& name) {
    auto i = ev.graph().find(fmt::format("prop:{}.{}", target, name));
    return i == Graph::kNone ? std::string() : ev.graph().node(i).error;
}

Slot clipSlot(const Clip& c, TrackKind track) {
    if (c.scriptOwned()) return Slot::ClipScript;
    switch (c.kind) {
    case ClipKind::Media: return track == TrackKind::Audio ? Slot::ClipAudio : Slot::ClipMedia;
    case ClipKind::Text: return Slot::ClipText;
    case ClipKind::Solid: return Slot::ClipSolid;
    case ClipKind::Path: return Slot::ClipGenerator;
    case ClipKind::Generator: return Slot::ClipGenerator;
    case ClipKind::Synth: return Slot::ClipAudio;
    }
    return Slot::ClipMedia;
}

namespace {

int decimalsFor(double step) {
    int d = 0;
    while (d < 4 && std::abs(step * std::pow(10.0, d) - std::round(step * std::pow(10.0, d))) > 1e-9) ++d;
    return d;
}

std::string valueText(const Value& v, const PropSpec& spec) {
    if (std::holds_alternative<double>(v)) return formatNumber(std::get<double>(v), std::max(2, decimalsFor(spec.step)));
    if (std::holds_alternative<Color>(v)) return formatColor(std::get<Color>(v));
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    return "-";
}

// A Lua literal for a value, used as the starting text of a new expression.
std::string luaLiteral(const Value& v) {
    if (std::holds_alternative<double>(v)) return formatNumber(std::get<double>(v), 4);
    if (std::holds_alternative<Color>(v)) return fmt::format("\"{}\"", formatColor(std::get<Color>(v)));
    if (std::holds_alternative<std::string>(v)) return fmt::format("\"{}\"", std::get<std::string>(v));
    return "0";
}

void keyDiamond(Context& ui, Vec c, float r, bool filled, bool hovered) {
    ui.painter.diamond(c, r, hovered ? ui.col(Slot::Text) : ui.col(Slot::Keyframe));
    if (!filled) ui.painter.diamond(c, r - ui.dp(1.5f), ui.col(Slot::Panel));
}

}  // namespace

bool editNumber(ViewEnv& env, const Rect& r, std::string_view id, double value, const NumberStyle& style,
                const std::string& label, const std::function<void(Project&, double)>& apply) {
    Session& s = env.session;
    DragResult d = numberField(env.ui, r, id, value, style);
    bool changed = false;
    try {
        if (d.began) s.beginGesture(label);
        if (d.committed) changed = env.shell.edit(label, [&](Project& p) { apply(p, value); });
        else if (d.changed) {
            s.updateGesture([&](Project& p) { apply(p, value); });
            changed = true;
        }
    } catch (const std::exception& e) {
        env.shell.notify(e.what(), true);
    }
    if (d.ended) s.endGesture();
    return changed;
}

void propertyRow(ViewEnv& env, const Rect& rowIn, Id target, const PropSpec& spec, float labelWidth, bool readOnly) {
    Context& ui = env.ui;
    Session& s = env.session;
    Shell& shell = env.shell;
    std::string name = spec.name;
    ui.pushId(name);
    Rect row = rowIn;
    const Project& project = readOnly ? s.evaluator().project() : s.project();
    // Copies: an edit in this row replaces the project.
    const Property* found = findProperty(project, target, name);
    const Property propCopy = found ? *found : Property::of(spec.def);
    const Property* prop = &propCopy;
    Evaluator& ev = s.evaluator();
    ev.setTime(s.time());
    Value current = evaluatedValue(ev, target, name);
    if (std::holds_alternative<std::monostate>(current)) current = prop->mode == PropMode::Constant ? prop->constant : spec.def;
    double localT = s.time() - targetStart(project, target);
    std::string error = propertyError(ev, target, name);

    // Label: clicking selects the property for the curves view.
    Rect label = cutLeft(row, labelWidth);
    bool selected = s.selection.prop == name && (s.selection.primaryClip() == target || s.selection.effect == target ||
                                                 s.selection.transition == target);
    bool number = spec.type == ValueType::Number;
    bool canKey = number && spec.animatable && !readOnly;
    Hit lh = ui.interact(ui.id("label"), label);
    if (!error.empty()) ui.tooltip(error);
    else if (canKey) ui.tooltip("Click to edit this property in the curves view");
    if (selected) ui.painter.fill(label, ui.col(Slot::Selection));
    else if (lh.hovered) ui.painter.fill(label, ui.col(Slot::Hover));
    ui.painter.textIn(label.inset(ui.dp(8), 0), name, error.empty() ? ui.col(Slot::TextDim) : ui.col(Slot::Error));
    if (lh.clicked) s.selection.prop = name;
    Rect key = cutRight(row, ui.dp(22));
    Rect mode = cutLeft(row, ui.dp(26));
    Rect editor = row.inset(0, ui.dp(2));
    editor.w -= ui.dp(4);

    if (readOnly) {
        ui.painter.textIn(editor.inset(ui.dp(6), 0), valueText(current, spec), ui.col(Slot::TextDim));
        ui.popId();
        return;
    }

    // Mode menu.
    const char* modeText = prop->mode == PropMode::Constant ? "=" : prop->mode == PropMode::Curve ? "K" : "fx";
    WidgetId menu = ui.id("mode-menu");
    bool openMode = button(ui, mode.inset(ui.dp(2)), fmt::format("{}##mode", modeText), prop->mode != PropMode::Constant);
    if (prop->mode == PropMode::Curve) ui.tooltip("Keyframes. Click to change the mode or edit the curve.");
    else if (prop->mode == PropMode::Expression) ui.tooltip("Lua expression. Click to change the mode.");
    else if (canKey) ui.tooltip("Constant. Click to use keyframes or an expression.");
    else if (spec.animatable) ui.tooltip("Constant. Click to use an expression.");
    else ui.tooltip("Constant. Click to reset to the default.");
    if (openMode) ui.openPopup(menu, {mode.x, mode.b()});
    if (beginMenu(ui, menu, ui.dp(170))) {
        if (menuItem(ui, "Constant", {}, true, prop->mode == PropMode::Constant) && prop->mode != PropMode::Constant)
            shell.edit("Set " + name + " constant", [&](Project& p) { edit::setPropConstant(p, target, name, current); });
        if (spec.animatable && number &&
            menuItem(ui, "Keyframes", {}, true, prop->mode == PropMode::Curve) && prop->mode != PropMode::Curve)
            shell.edit("Keyframe " + name, [&](Project& p) { edit::setKeyframe(p, target, name, localT, asNumber(current)); });
        if (spec.animatable &&
            menuItem(ui, "Expression", {}, true, prop->mode == PropMode::Expression) && prop->mode != PropMode::Expression)
            shell.edit("Set " + name + " expression", [&](Project& p) { edit::setPropExpr(p, target, name, luaLiteral(current)); });
        if (prop->mode == PropMode::Curve && menuItem(ui, "Edit curve"))
        {
            s.selection.prop = name;
            shell.showView("curves");
        }
        menuSeparator(ui);
        if (menuItem(ui, "Reset to default"))
            shell.edit("Reset " + name, [&](Project& p) { edit::setPropConstant(p, target, name, spec.def); });
        endMenu(ui);
    }

    // Value editor.
    if (prop->mode == PropMode::Expression) {
        std::string expr = prop->expr;
        Rect valueArea = cutRight(editor, std::min(editor.w * 0.35f, ui.dp(90)));
        if (textField(ui, editor, "##expr", expr, FontRole::Mono).committed)
            shell.edit("Set " + name + " expression", [&](Project& p) { edit::setPropExpr(p, target, name, expr); });
        ui.painter.textIn(valueArea.inset(ui.dp(6), 0), valueText(current, spec), error.empty() ? ui.col(Slot::TextDim) : ui.col(Slot::Error),
                          Align::Right);
    } else if (number) {
        double v = asNumber(current);
        NumberStyle style{.step = spec.step, .min = spec.min, .max = spec.max, .decimals = std::max(2, decimalsFor(spec.step)),
                          .unit = spec.unit};
        if (spec.min >= spec.max) style.min = -std::numeric_limits<double>::infinity(), style.max = std::numeric_limits<double>::infinity();
        bool curve = prop->mode == PropMode::Curve;
        editNumber(env, editor, "##num", v, style, "Set " + name, [&](Project& p, double nv) {
            if (curve) edit::setKeyframe(p, target, name, localT, nv);
            else edit::setPropConstant(p, target, name, nv);
        });
    } else if (spec.type == ValueType::Color) {
        Color c = asColor(current);
        Rect swatch = cutLeft(editor, editor.h);
        ui.painter.fill(swatch, c);
        ui.painter.outline(swatch, ui.col(Slot::Border));
        cutLeft(editor, ui.dp(4));
        std::string hex = formatColor(c);
        if (textField(ui, editor, "##color", hex, FontRole::Mono).committed) {
            if (auto parsed = parseColor(hex))
                shell.edit("Set " + name, [&](Project& p) { edit::setPropConstant(p, target, name, *parsed); });
            else shell.notify(fmt::format("Cannot parse color \"{}\"", hex), true);
        }
    } else if (!spec.choices.empty()) {
        std::vector<std::string> options(spec.choices.begin(), spec.choices.end());
        std::string cur = asString(current);
        int index = 0;
        for (size_t i = 0; i < options.size(); ++i)
            if (options[i] == cur) index = int(i);
        if (dropdown(ui, editor, "##choice", index, options))
            shell.edit("Set " + name, [&](Project& p) { edit::setPropConstant(p, target, name, options[index]); });
    } else {
        std::string text = asString(current);
        if (textField(ui, editor, "##text", text).committed)
            shell.edit("Set " + name, [&](Project& p) { edit::setPropConstant(p, target, name, text); });
    }

    // Keyframe toggle at the playhead.
    if (canKey) {
        double fps = s.project().settings.fps > 0 ? s.project().settings.fps : 30;
        bool atKey = false;
        if (prop->mode == PropMode::Curve)
            for (const auto& k : prop->curve.keys) atKey |= std::abs(k.t - localT) < 0.5 / fps;
        Hit kh = ui.interact(ui.id("key"), key);
        ui.tooltip(atKey ? "Remove the keyframe at the playhead" : "Add a keyframe at the playhead");
        Vec center{key.x + key.w / 2, key.y + key.h / 2};
        if (prop->mode == PropMode::Curve || kh.hovered) keyDiamond(ui, center, ui.dp(5), atKey, kh.hovered);
        if (kh.clicked) {
            if (atKey) {
                double kt = localT;
                for (const auto& k : prop->curve.keys)
                    if (std::abs(k.t - localT) < 0.5 / fps) kt = k.t;
                shell.edit("Remove keyframe", [&](Project& p) { edit::removeKeyframe(p, target, name, kt); });
            } else {
                double v = asNumber(current);
                shell.edit("Add keyframe", [&](Project& p) { edit::setKeyframe(p, target, name, localT, v); });
            }
        }
    }
    ui.popId();
}

Rect labeled(Context& ui, Rect row, std::string_view label, float labelWidth) {
    Rect l = cutLeft(row, labelWidth);
    ui.painter.textIn(l.inset(ui.dp(8), 0), label, ui.col(Slot::TextDim));
    Rect editor = row.inset(0, ui.dp(2));
    editor.w = std::max(0.0f, editor.w - ui.dp(4));
    return editor;
}

Rect sectionHeader(Context& ui, Rect& area, std::string_view title) {
    Rect r = cutTop(area, ui.rowHeight());
    ui.painter.fill(r, ui.col(Slot::Panel));
    Rect t = r.inset(ui.dp(8), 0);
    float w = ui.painter.measure(title);
    ui.painter.textIn(cutLeft(t, w), title, ui.col(Slot::Text));
    return t;
}

bool startPanel(ViewEnv& env, const Rect& area, std::string_view title) {
    constexpr size_t kRecentRows = 5;
    Context& ui = env.ui;
    Shell& shell = env.shell;
    const std::vector<std::string>& recent = shell.recentProjects();
    size_t rows = std::min(recent.size(), kRecentRows);
    float row = ui.rowHeight(), pad = ui.dp(16);
    float w = std::min(area.w - ui.dp(16), ui.dp(400));
    float h = pad * 2 + row * 2 + ui.dp(8) + (rows > 0 ? row * float(rows + 1) + ui.dp(8) : 0);
    if (w < ui.dp(160) || h > area.h) return false;
    Rect box{std::round(area.x + (area.w - w) / 2), std::round(area.y + (area.h - h) / 2), w, h};
    ui.interact(ui.id("##start"), box);  // the panel covers what is behind it
    ui.painter.fill(box, ui.col(Slot::Panel));
    ui.painter.outline(box, ui.col(Slot::Border));
    Rect in = box.inset(pad);
    ui.painter.textIn(cutTop(in, row), title, ui.col(Slot::TextDim));
    cutTop(in, ui.dp(8));
    Rect buttons = cutTop(in, row);
    float bw = (buttons.w - ui.dp(8)) / 2;
    bool import = button(ui, cutLeft(buttons, bw), "Import media...##start", true);
    ui.tooltip(shell.withShortcut("Add video, image, or audio files to the project", "file.import"));
    if (import) shell.importMedia();
    bool open = button(ui, cutRight(buttons, bw), "Open project...##start");
    ui.tooltip(shell.withShortcut("Open a project file", "file.open"));
    if (open) shell.openProject();
    if (rows == 0) return true;
    cutTop(in, ui.dp(8));
    ui.painter.textIn(cutTop(in, row), "Recent projects", ui.col(Slot::TextFaint));
    std::string chosen;
    for (size_t i = 0; i < rows; ++i) {
        const std::string& path = recent[i];
        std::filesystem::path fp(path);
        Rect r = cutTop(in, row);
        Hit hit = ui.interact(ui.id("##recent-" + path), r);
        if (hit.hovered) ui.painter.fill(r, ui.col(Slot::Hover));
        Rect t = r.inset(ui.dp(6), 0);
        std::string name = fp.filename().string();
        ui.painter.textIn(cutLeft(t, std::min(t.w, ui.painter.measure(name) + ui.dp(12))), name, ui.col(Slot::Text));
        ui.painter.textIn(t, shortPath(fp.parent_path().string()), ui.col(Slot::TextFaint));
        ui.tooltip("Open " + shortPath(path));
        if (hit.clicked) chosen = path;
    }
    if (!chosen.empty()) shell.openProjectFile(chosen);
    return true;
}

float paragraph(Context& ui, const Rect& area, std::string_view text, Slot color) {
    float y = area.y, lh = ui.painter.lineHeight();
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        ui.painter.textIn({area.x, y, area.w, lh}, text.substr(start, end - start), ui.col(color));
        y += lh;
        start = end + 1;
    }
    return y - area.y;
}

ListLayout::ListLayout(Context& ui, const Rect& view, std::string_view id, float contentHeight) : ui_(ui), view_(view) {
    y_ = view.y - beginScroll(ui, view, id, contentHeight);
}

ListLayout::~ListLayout() { endScroll(ui_); }

Rect ListLayout::next(float h) {
    Rect r{view_.x, y_, view_.w, h};
    y_ += h;
    return r;
}

}  // namespace mf::ui::views
