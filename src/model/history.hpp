#pragma once

#include "model/project.hpp"

#include <string>
#include <vector>

namespace mf {

// Snapshot undo. Each entry holds the project state before an edit.
// Projects hold no media data, so snapshots stay small.
class History {
public:
    explicit History(size_t limit = 500) : limit_(limit) {}

    void push(std::string label, Project before);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // Swaps `current` with the stored state. Returns the label of the step.
    std::string undo(Project& current);
    std::string redo(Project& current);
    void clear();

    std::vector<std::string> undoLabels() const;
    std::vector<std::string> redoLabels() const;

private:
    struct Entry {
        std::string label;
        Project state;
    };
    size_t limit_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
};

}  // namespace mf
