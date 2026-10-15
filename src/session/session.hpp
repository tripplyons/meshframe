#pragma once

#include "audio/mixer.hpp"
#include "engine/evaluator.hpp"
#include "engine/frame_renderer.hpp"
#include "engine/player.hpp"
#include "exporter/exporter.hpp"
#include "media/media_cache.hpp"
#include "model/history.hpp"
#include "model/project.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mf {

struct LogLine {
    enum class Level { Info, Warn, Error } level = Level::Info;
    std::string text;
};

// What the user (or an agent) has selected. Views read and write it.
struct Selection {
    std::vector<Id> clips;
    Id effect = 0;
    Id transition = 0;
    Id track = 0;
    Id media = 0;
    std::string prop;  // property name on the primary target, for the curve editor

    Id primaryClip() const { return clips.empty() ? 0 : clips.front(); }
    bool hasClip(Id id) const;
};

// One open project with its history, evaluator, media, transport, and export
// job. The GUI, the socket server, and headless CLI runs all drive a Session.
class Session {
public:
    // With a sink, playback produces audio; without, the wall clock drives it.
    explicit Session(std::unique_ptr<AudioSink> sink = nullptr);
    ~Session();

    const Project& project() const { return project_; }
    // Effective project (with script clips), synced with the latest edits.
    Evaluator& evaluator();
    uint64_t revision() const { return revision_; }

    // Runs fn on a copy of the project; commits it as one undo step if fn
    // returns normally. EditError and other exceptions propagate unchanged.
    void apply(const std::string& label, const std::function<void(Project&)>& fn);
    // Continuous edits (drags): one undo step for many updates.
    void beginGesture(const std::string& label);
    void updateGesture(const std::function<void(Project&)>& fn);
    void endGesture();
    bool inGesture() const { return gesture_.has_value(); }

    // Groups the edits made until commitBatch into one undo step.
    // rollbackBatch restores the project, history, and selection instead.
    void beginBatch();
    // Returns whether the batch changed the project and added an undo step.
    bool commitBatch(const std::string& label);
    void rollbackBatch();

    bool undo();
    bool redo();
    const History& history() const { return history_; }

    // Files. `newProject` resets to the default project.
    void newProject();
    bool open(const std::string& path, std::string* err);
    bool save(const std::string& path, std::string* err);
    // Opens a recovery copy as unsaved changes to the project at `path`
    // ("" for a project that was never saved).
    bool recover(const std::string& copy, const std::string& path, std::string* err);
    const std::string& path() const { return path_; }
    bool dirty() const { return revision_ != savedRevision_; }

    // Probes a file and adds it to the media pool. Returns 0 and sets err on failure.
    Id importMedia(const std::string& path, std::string* err);
    MediaCache& cache() { return *cache_; }

    // Transport.
    Player& player() { return *player_; }
    double time() const { return player_->time(); }
    void seek(double t);
    void togglePlay();
    // Advances playback and background work; call once per UI frame.
    double tick();

    // Renders a frame at time t; w x h may be smaller than the project size.
    std::shared_ptr<Image> renderFrame(double t, int w, int h);

    ExportJob& exportJob() { return exportJob_; }

    Selection selection;
    void pruneSelection();

    // Log for the console view and the CLI.
    void log(LogLine::Level level, std::string text);
    const std::deque<LogLine>& logLines() const { return log_; }
    uint64_t logCount() const { return logCount_; }

    // Called after each frame tick; servers use it to finish deferred replies.
    std::vector<std::function<bool()>> pollers;

private:
    void changed();

    Project project_;
    History history_;
    std::optional<Project> gesture_;
    std::string gestureLabel_;
    bool gestureChanged_ = false;
    struct BatchStart {
        Project project;
        History history;
        Selection selection;
        uint64_t revision;
        bool clean;
    };
    std::optional<BatchStart> batch_;
    uint64_t revision_ = 1, savedRevision_ = 1, evaluated_ = 0;
    std::string path_;
    std::unique_ptr<MediaCache> cache_;
    std::unique_ptr<Evaluator> evaluator_;
    std::unique_ptr<FrameRenderer> renderer_;
    std::unique_ptr<AudioMixer> mixer_;
    std::unique_ptr<AudioSink> sink_;
    std::unique_ptr<Player> player_;
    ExportJob exportJob_;
    std::deque<LogLine> log_;
    uint64_t logCount_ = 0;
};

}  // namespace mf
