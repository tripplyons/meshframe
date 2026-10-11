#pragma once

#include "ui/view.hpp"

#include <memory>

namespace mf::ui::views {

// Placeholder for a view type that has no implementation yet.
std::unique_ptr<View> placeholder(std::string label);

std::unique_ptr<View> preview();
std::unique_ptr<View> timeline();
std::unique_ptr<View> media();
std::unique_ptr<View> inspector();
std::unique_ptr<View> videoEffects();
std::unique_ptr<View> audioEffects();
std::unique_ptr<View> curves();
std::unique_ptr<View> script();
std::unique_ptr<View> graph();
std::unique_ptr<View> meters();
std::unique_ptr<View> scopes();
std::unique_ptr<View> console();
std::unique_ptr<View> history();
std::unique_ptr<View> exportView();
std::unique_ptr<View> settings();

}  // namespace mf::ui::views
