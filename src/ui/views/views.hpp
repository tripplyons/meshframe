#pragma once

#include "ui/view.hpp"

#include <memory>

namespace mf::ui::views {

// Placeholder for a view type that has no implementation yet.
std::unique_ptr<View> placeholder(std::string label);

}  // namespace mf::ui::views
