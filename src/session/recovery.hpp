#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace mf {

class Session;

// Copies of unsaved work, so a crash loses at most a few seconds of edits.
// Each editor keeps one copy in the directory and holds a lock on it while it
// runs. A copy without a lock was left by an editor that did not quit
// normally.
class Recovery {
public:
    struct Copy {
        std::string file;  // the copied project
        std::string path;  // the project's own file, or "" if it was never saved
        std::string name;
        std::time_t savedAt = 0;
    };

    explicit Recovery(std::string dir);
    // Removes this editor's copy.
    ~Recovery();
    Recovery(const Recovery&) = delete;
    Recovery& operator=(const Recovery&) = delete;

    // Copies the project when it has changes the copy does not have, at most
    // once per interval. Removes the copy when nothing is unsaved.
    void update(const Session& s, double now);
    // Copies left by editors that are no longer running, newest first.
    std::vector<Copy> orphans() const;
    // Deletes a copy and its details.
    static void remove(const Copy& c);

    static constexpr double kInterval = 10;  // seconds between copies

private:
    void clear();

    std::string dir_;
    std::string stem_;  // this editor's files: <dir>/<stem>.mfproj, .json, .lock
    int lockFd_ = -1;
    bool hasCopy_ = false;
    uint64_t copied_ = 0;
    double copiedAt_ = -1e300;
};

}  // namespace mf
