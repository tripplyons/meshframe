#include "model/history.hpp"

namespace mf {

void History::push(std::string label, Project before) {
    push(std::move(label), std::make_shared<const Project>(std::move(before)));
}

void History::push(std::string label, std::shared_ptr<const Project> before) {
    undo_.push_back({std::move(label), std::move(before)});
    if (undo_.size() > limit_) undo_.erase(undo_.begin());
    redo_.clear();
}

std::string History::undo(Project& current) {
    if (!canUndo()) return {};
    auto state = std::make_shared<const Project>(current);
    auto label = undo(state);
    current = *state;
    return label;
}

std::string History::redo(Project& current) {
    if (!canRedo()) return {};
    auto state = std::make_shared<const Project>(current);
    auto label = redo(state);
    current = *state;
    return label;
}

std::string History::undo(std::shared_ptr<const Project>& current) {
    if (undo_.empty()) return {};
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({e.label, std::move(current)});
    current = std::move(e.state);
    return e.label;
}

std::string History::redo(std::shared_ptr<const Project>& current) {
    if (redo_.empty()) return {};
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({e.label, std::move(current)});
    current = std::move(e.state);
    return e.label;
}

void History::clear() {
    undo_.clear();
    redo_.clear();
}

std::vector<std::string> History::undoLabels() const {
    std::vector<std::string> out;
    for (const auto& e : undo_) out.push_back(e.label);
    return out;
}

std::vector<std::string> History::redoLabels() const {
    std::vector<std::string> out;
    for (const auto& e : redo_) out.push_back(e.label);
    return out;
}

}  // namespace mf
