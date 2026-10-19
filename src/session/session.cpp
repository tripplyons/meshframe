#include "session/session.hpp"

#include "media/decoder.hpp"
#include "model/edit.hpp"
#include "model/serialize.hpp"
#include "render/compositor.hpp"

#include <algorithm>
#include <filesystem>

namespace mf {

bool Selection::hasClip(Id id) const { return std::ranges::contains(clips, id); }

Session::Session(std::unique_ptr<AudioSink> sink)
    : project_(std::make_shared<const Project>(Project::makeDefault())),
      cache_(std::make_unique<MediaCache>()),
      evaluator_(std::make_unique<Evaluator>()),
      renderer_(std::make_unique<FrameRenderer>(*cache_)),
      mixer_(std::make_unique<AudioMixer>(*cache_)),
      sink_(std::move(sink)),
      player_(std::make_unique<Player>(*mixer_, sink_.get())) {
    evaluator_->onLog = [this](std::string line) { log(LogLine::Level::Info, std::move(line)); };
    revisions_.push_back({revision_, "new", source_, project_});
}

Session::~Session() {
    exportJob_.cancel();
    exportJob_.wait();
}

Evaluator& Session::evaluator() {
    if (evaluated_ != revision_) {
        std::string before = evaluator_->script().error ? evaluator_->script().error->message : "";
        evaluator_->setProject(*project_);
        evaluated_ = revision_;
        const auto& err = evaluator_->script().error;
        if (err && err->message != before) log(LogLine::Level::Error, "script: " + err->message);
    }
    return *evaluator_;
}

void Session::changed(const std::string& label) {
    ++revision_;
    pruneSelection();
    revisions_.push_back({revision_, label, source_, project_});
    while (revisions_.size() > kRevisionsKept) revisions_.pop_front();
}

std::string Session::setSource(std::string source) {
    std::swap(source, source_);
    return source;
}

void Session::apply(const std::string& label, const std::function<void(Project&)>& fn) {
    if (gesture_) endGesture();
    Project next = *project_;
    fn(next);
    history_.push(label, std::move(project_));
    project_ = std::make_shared<const Project>(std::move(next));
    changed(label);
}

void Session::beginGesture(const std::string& label) {
    if (gesture_) endGesture();
    gesture_ = project_;
    gestureLabel_ = label;
    gestureChanged_ = false;
}

void Session::updateGesture(const std::function<void(Project&)>& fn) {
    if (!gesture_) beginGesture("edit");
    Project next = *project_;
    fn(next);
    project_ = std::make_shared<const Project>(std::move(next));
    gestureChanged_ = true;
    changed(gestureLabel_);
}

void Session::endGesture() {
    if (!gesture_) return;
    if (gestureChanged_) history_.push(gestureLabel_, std::move(*gesture_));
    gesture_.reset();
}

void Session::beginBatch() {
    if (gesture_) endGesture();
    batch_ = BatchStart{project_, history_, selection, revision_, !dirty()};
}

bool Session::commitBatch(const std::string& label) {
    if (!batch_) return false;
    if (gesture_) endGesture();
    bool edited = revision_ != batch_->revision;
    if (edited) {
        history_ = std::move(batch_->history);
        history_.push(label, std::move(batch_->project));
    }
    batch_.reset();
    return edited;
}

void Session::rollbackBatch() {
    if (!batch_) return;
    gesture_.reset();
    bool edited = revision_ != batch_->revision;
    project_ = std::move(batch_->project);
    history_ = std::move(batch_->history);
    selection = std::move(batch_->selection);
    if (edited) {
        changed("rollback");
        if (batch_->clean) savedRevision_ = revision_;
    }
    batch_.reset();
}

bool Session::undo() {
    if (gesture_) endGesture();
    if (!history_.canUndo()) return false;
    std::string label = history_.undo(project_);
    changed("undo: " + label);
    log(LogLine::Level::Info, "undo: " + label);
    return true;
}

bool Session::redo() {
    if (gesture_) endGesture();
    if (!history_.canRedo()) return false;
    std::string label = history_.redo(project_);
    changed("redo: " + label);
    log(LogLine::Level::Info, "redo: " + label);
    return true;
}

void Session::newProject() {
    player_->pause();
    project_ = std::make_shared<const Project>(Project::makeDefault());
    history_.clear();
    path_.clear();
    selection = {};
    changed("new");
    savedRevision_ = revision_;
}

bool Session::open(const std::string& path, std::string* err) {
    try {
        Project p = loadProject(path);
        player_->pause();
        project_ = std::make_shared<const Project>(std::move(p));
        history_.clear();
        path_ = std::filesystem::absolute(path).string();
        selection = {};
        player_->seek(0);
        changed("open");
        savedRevision_ = revision_;
        log(LogLine::Level::Info, "opened " + path_);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool Session::recover(const std::string& copy, const std::string& path, std::string* err) {
    if (!open(copy, err)) return false;
    path_ = path;
    savedRevision_ = 0;  // revisions start at 1, so the project stays unsaved
    log(LogLine::Level::Info, "recovered unsaved changes" + (path.empty() ? std::string() : " to " + path));
    return true;
}

bool Session::save(const std::string& path, std::string* err) {
    std::string target = path.empty() ? path_ : path;
    if (target.empty()) {
        if (err) *err = "no path: give a file name";
        return false;
    }
    try {
        saveProject(*project_, target);
        path_ = std::filesystem::absolute(target).string();
        savedRevision_ = revision_;
        log(LogLine::Level::Info, "saved " + path_);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

Id Session::importMedia(const std::string& path, std::string* err) {
    MediaSource m;
    std::string abs = std::filesystem::absolute(path).string();
    if (!probeMedia(abs, m, err)) return 0;
    Id id = 0;
    try {
        apply("import " + m.name, [&](Project& p) { id = edit::addMedia(p, m); });
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return 0;
    }
    if (m.hasAudio) cache_->audio(*project_->findMedia(id), project_->settings.sampleRate);  // decode in background
    log(LogLine::Level::Info, "imported " + abs);
    return id;
}

void Session::seek(double t) { player_->seek(std::max(0.0, t)); }

void Session::togglePlay() {
    if (player_->playing()) player_->pause();
    else player_->play(evaluator());
}

double Session::tick() {
    double t = player_->update(evaluator());
    std::erase_if(pollers, [](auto& poll) { return poll(); });
    return t;
}

std::shared_ptr<Image> Session::renderFrame(double t, int w, int h) {
    auto frame = renderer_->render(evaluator(), t, w, h);
    previewFrame_ = frame;
    previewRevision_ = revision_;
    previewTime_ = t;
    return frame;
}

ImagePtr Session::analysisFrame(double t, int w, int h) {
    if (!previewFrame_ || previewRevision_ != revision_ || previewTime_ != t ||
        previewFrame_->w < w || previewFrame_->h < h) return renderFrame(t, w, h);
    if (previewFrame_->w == w && previewFrame_->h == h) return previewFrame_;
    auto reduced = std::make_shared<Image>(w, h);
    Placement place{w / 2.0, h / 2.0, double(w) / previewFrame_->w, double(h) / previewFrame_->h};
    drawLayer(*reduced, *previewFrame_, place, 1, Blend::Normal);
    return reduced;
}

std::shared_ptr<Image> Session::renderClip(const Clip& c, double t, double scale, int padX, int padY) {
    return renderer_->renderClip(evaluator(), c, t, scale, padX, padY);
}

void Session::pruneSelection() {
    std::erase_if(selection.clips, [&](Id id) { return !project_->findClip(id) && id < kScriptIdBase; });
    if (selection.transition && !project_->findTransition(selection.transition)) selection.transition = 0;
    if (selection.track && !project_->findTrack(selection.track)) selection.track = 0;
    if (selection.media && !project_->findMedia(selection.media)) selection.media = 0;
    if (selection.effect && !project_->findEffect(selection.effect)) selection.effect = 0;
}

void Session::log(LogLine::Level level, std::string text) {
    log_.push_back({level, std::move(text)});
    ++logCount_;
    while (log_.size() > 2000) log_.pop_front();
}

}  // namespace mf
