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
        add("preview", "Preview", views::preview);
        add("timeline", "Timeline", views::timeline);
        add("media", "Media", views::media);
        add("inspector", "Inspector", views::inspector);
        add("video-effects", "Video effects", views::videoEffects);
        add("audio-effects", "Audio effects", views::audioEffects);
        add("curves", "Curves", views::curves);
        add("script", "Script", views::script);
        add("graph", "Graph", views::graph);
        add("meters", "Meters", views::meters);
        add("scopes", "Scopes", views::scopes);
        add("console", "Console", views::console);
        add("history", "History", views::history);
        add("export", "Export", views::exportView);
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
