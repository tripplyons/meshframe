#include "session/recovery.hpp"

#include "model/serialize.hpp"
#include "session/session.hpp"

#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

namespace mf {

namespace fs = std::filesystem;

namespace {

// A name for this editor's files. The random part keeps a later editor that
// gets the same process id from taking over the copy of a crashed one.
std::string uniqueStem() {
    std::random_device rd;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d-%08x", int(::getpid()), unsigned(rd()));
    return buf;
}

// True when no running editor holds the lock file.
bool unlocked(const fs::path& lock) {
    int fd = ::open(lock.c_str(), O_RDWR);
    if (fd < 0) return true;
    bool free = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    ::close(fd);
    return free;
}

}  // namespace

Recovery::Recovery(std::string dir) : dir_(std::move(dir)), stem_(uniqueStem()) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    fs::path lock = fs::path(dir_) / (stem_ + ".lock");
    lockFd_ = ::open(lock.c_str(), O_RDWR | O_CREAT, 0644);
    if (lockFd_ >= 0 && ::flock(lockFd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(lockFd_);
        lockFd_ = -1;
    }
}

Recovery::~Recovery() {
    clear();
    if (lockFd_ < 0) return;
    std::error_code ec;
    fs::remove(fs::path(dir_) / (stem_ + ".lock"), ec);
    ::close(lockFd_);
}

void Recovery::update(const Session& s, double now) {
    if (lockFd_ < 0) return;
    if (!s.dirty()) return clear();
    if (s.revision() == copied_ || now - copiedAt_ < kInterval) return;
    copied_ = s.revision();
    copiedAt_ = now;
    fs::path base = fs::path(dir_) / stem_;
    try {
        saveProject(s.project(), base.string() + ".mfproj");
        nlohmann::json info = {{"path", s.path()}, {"name", s.project().name}, {"savedAt", std::time(nullptr)}};
        std::ofstream out(base.string() + ".json", std::ios::trunc);
        out << info.dump(2) << '\n';
        hasCopy_ = true;
    } catch (const std::exception&) {
        // A full disk or a removed directory: try again after the next interval.
    }
}

void Recovery::clear() {
    if (!hasCopy_) return;
    remove({(fs::path(dir_) / (stem_ + ".mfproj")).string(), {}, {}, 0});
    hasCopy_ = false;
    copied_ = 0;
    copiedAt_ = -1e300;
}

std::vector<Recovery::Copy> Recovery::orphans() const {
    std::vector<Copy> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        fs::path p = entry.path();
        if (p.extension() != ".json" || p.stem() == stem_) continue;
        fs::path file = fs::path(p).replace_extension(".mfproj");
        if (!fs::exists(file) || !unlocked(fs::path(p).replace_extension(".lock"))) continue;
        std::ifstream in(p);
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) continue;
        out.push_back({file.string(), j.value("path", ""), j.value("name", ""), j.value("savedAt", std::time_t(0))});
    }
    std::ranges::sort(out, [](const Copy& a, const Copy& b) { return a.savedAt > b.savedAt; });
    return out;
}

void Recovery::remove(const Copy& c) {
    std::error_code ec;
    fs::path file(c.file);
    fs::remove(file, ec);
    fs::remove(fs::path(file).replace_extension(".json"), ec);
    // A lock file without a holder is left from a crash; this editor's own lock stays.
    fs::path lock = fs::path(file).replace_extension(".lock");
    if (unlocked(lock)) fs::remove(lock, ec);
}

}  // namespace mf
