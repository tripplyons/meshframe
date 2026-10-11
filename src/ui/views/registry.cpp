#include "ui/views/views.hpp"

#include <fmt/format.h>

namespace mf::ui {

namespace views {

namespace {

class Placeholder : public View {
public:
    explicit Placeholder(std::string label) : label_(std::move(label)) {}
    void draw(ViewEnv& env, const Rect& body) override {
        env.ui.painter.textIn(body, label_, env.ui.col(Slot::TextFaint), Align::Center);
    }

private:
    std::string label_;
};

}  // namespace

std::unique_ptr<View> placeholder(std::string label) { return std::make_unique<Placeholder>(std::move(label)); }

}  // namespace views

const std::vector<ViewType>& viewTypes() {
    static const std::vector<ViewType> types = [] {
        std::vector<ViewType> t;
        auto add = [&](std::string id, std::string label, std::function<std::unique_ptr<View>()> make = {}) {
            if (!make) make = [label] { return views::placeholder(label); };
            t.push_back({std::move(id), std::move(label), std::move(make)});
        };
        add("preview", "Preview");
        add("timeline", "Timeline");
        add("media", "Media");
        add("inspector", "Inspector");
        add("video-effects", "Video effects");
        add("audio-effects", "Audio effects");
        add("curves", "Curves");
        add("script", "Script");
        add("graph", "Graph");
        add("meters", "Meters");
        add("scopes", "Scopes");
        add("console", "Console");
        add("history", "History");
        add("export", "Export");
        add("settings", "Settings");
        return t;
    }();
    return types;
}

const ViewType* findViewType(std::string_view id) {
    for (const auto& v : viewTypes())
        if (v.id == id) return &v;
    return nullptr;
}

}  // namespace mf::ui
