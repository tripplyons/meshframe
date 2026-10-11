#pragma once

#include "session/commands.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mf {

// Where running sessions publish their sockets: $MESHFRAME_RUNTIME_DIR, or
// /tmp/meshframe-<uid>.
std::string runtimeDir();

// A running session found in the runtime directory.
struct SessionInfo {
    int pid = 0;
    std::string socket;
    std::string infoPath;
};

// Sessions whose process is alive. Info files of dead processes are removed.
std::vector<SessionInfo> listSessions();

// Serves a command set over a Unix socket. Clients send one JSON request per
// line and get one JSON reply per line. An I/O thread accepts connections and
// reads lines; requests run on the owner's thread in poll(), so commands never
// race the UI.
class CommandServer {
public:
    // `wake` is called from the I/O thread when a request arrives.
    CommandServer(Commands& commands, std::function<void()> wake);
    ~CommandServer();

    // Listens on `socketPath`. With `publish`, also writes <pid>.json next to
    // it so clients can find the session; stop() removes both files.
    bool start(const std::string& socketPath, bool publish, std::string* err);
    // Listens on <runtimeDir>/<pid>.sock and publishes it.
    bool startDefault(std::string* err);
    void stop();
    const std::string& path() const { return path_; }

    // Runs queued requests on the calling thread.
    void poll();

    struct Connection;

private:
    void ioLoop();

    Commands& commands_;
    std::function<void()> wake_;
    std::string path_, infoPath_;
    int listenFd_ = -1;
    int stopPipe_[2] = {-1, -1};
    std::thread thread_;

    std::mutex mu_;
    std::vector<std::pair<std::shared_ptr<Connection>, std::string>> queue_;
};

}  // namespace mf
