#include "ui/views/common.hpp"
#include "ui/views/views.hpp"

#include <fmt/format.h>

namespace mf::ui::views {

namespace {

// Undo history, oldest first. Clicking an entry undoes or redoes up to it.
class HistoryView : public View {
public:
    void draw(ViewEnv& env, const Rect& body) override {
        Context& ui = env.ui;
        Session& s = env.session;
        std::vector<std::string> undo = s.history().undoLabels();
        std::vector<std::string> redo = s.history().redoLabels();
        std::reverse(redo.begin(), redo.end());  // next redo first
        float rowH = ui.rowHeight();
        size_t total = 1 + undo.size() + redo.size();
        if (s.revision() != seenRevision_) {
            seenRevision_ = s.revision();
            scrollTo(ui, "##history", rowH * float(undo.size()), rowH, body.h);
        }
        int target = -1;  // entries applied after the click: 0 = the initial state
        {
            ListLayout list(ui, body, "##history", rowH * float(total));
            for (size_t i = 0; i < total; ++i) {
                Rect r = list.next(rowH);
                if (!list.visible(r)) continue;
                bool future = i > undo.size();
                std::string label = i == 0 ? "Initial state" : future ? redo[i - undo.size() - 1] : undo[i - 1];
                ui.pushId(uint64_t(i));
                Hit h;
                selectable(ui, r, "##entry", i == undo.size(), &h);
                ui.painter.textIn(r.inset(ui.dp(8), 0), label, ui.col(future ? Slot::TextFaint : Slot::Text));
                if (h.clicked) target = int(i);
                ui.popId();
            }
        }
        if (target < 0) return;
        int current = int(undo.size());
        for (int i = current; i > target; --i) s.undo();
        for (int i = current; i < target; ++i) s.redo();
    }

private:
    uint64_t seenRevision_ = 0;
};

}  // namespace

std::unique_ptr<View> history() { return std::make_unique<HistoryView>(); }

}  // namespace mf::ui::views
