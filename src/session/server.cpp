#include "session/server.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace mf {

namespace fs = std::filesystem;

std::string runtimeDir() {
    if (const char* dir = std::getenv("MESHFRAME_RUNTIME_DIR"); dir && *dir) return dir;
    return fmt::format("/tmp/meshframe-{}", getuid());
}

std::vector<SessionInfo> listSessions() {
    std::vector<SessionInfo> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(runtimeDir(), ec)) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream in(entry.path());
        json j = json::parse(in, nullptr, false);
        if (j.is_discarded() || !j.contains("pid") || !j.contains("socket")) continue;
        SessionInfo info{j["pid"].get<int>(), j.value("kind", "editor"), j["socket"].get<std::string>(), entry.path().string()};
        if (kill(info.pid, 0) != 0 && errno == ESRCH) {
            fs::remove(info.infoPath, ec);
            fs::remove(info.socket, ec);
            continue;
        }
        out.push_back(std::move(info));
    }
    std::ranges::sort(out, {}, &SessionInfo::pid);
    return out;
}

// One client. The I/O thread reads; replies are written from the owner's
// thread. A failed write marks the connection closed.
struct CommandServer::Connection {
    int fd = -1;
    std::string buffer;
    std::mutex writeMu;
    bool open = true;

    ~Connection() {
        if (fd >= 0) close(fd);
    }
    void send(const json& reply) {
        std::string line = reply.dump() + "\n";
        std::lock_guard lock(writeMu);
        size_t sent = 0;
        while (open && sent < line.size()) {
            ssize_t n = ::send(fd, line.data() + sent, line.size() - sent, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                open = false;
                break;
            }
            sent += size_t(n);
        }
    }
};

CommandServer::CommandServer(Commands& commands, std::function<void()> wake, std::string kind)
    : commands_(commands), wake_(std::move(wake)), kind_(std::move(kind)) {}

CommandServer::~CommandServer() { stop(); }

bool CommandServer::startDefault(std::string* err) {
    std::string dir = runtimeDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    chmod(dir.c_str(), 0700);
    return start(fmt::format("{}/{}.sock", dir, getpid()), true, err);
}

bool CommandServer::start(const std::string& socketPath, bool publish, std::string* err) {
    auto fail = [&](std::string msg) {
        if (err) *err = std::move(msg);
        if (listenFd_ >= 0) close(listenFd_), listenFd_ = -1;
        return false;
    };
    if (listenFd_ >= 0) return fail("the server is already running");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(addr.sun_path)) return fail("socket path is too long: " + socketPath);
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
    // Clients get EPIPE as an error, not a signal.
    std::signal(SIGPIPE, SIG_IGN);

    listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0) return fail(fmt::format("socket: {}", std::strerror(errno)));
    unlink(socketPath.c_str());
    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        return fail(fmt::format("cannot bind {}: {}", socketPath, std::strerror(errno)));
    chmod(socketPath.c_str(), 0600);
    if (listen(listenFd_, 16) != 0) return fail(fmt::format("listen: {}", std::strerror(errno)));
    if (pipe(stopPipe_) != 0) return fail(fmt::format("pipe: {}", std::strerror(errno)));
    path_ = socketPath;

    if (publish) {
        infoPath_ = fs::path(socketPath).replace_extension(".json").string();
        std::ofstream out(infoPath_);
        out << json{{"pid", getpid()}, {"kind", kind_}, {"socket", socketPath}}.dump() << "\n";
    }
    thread_ = std::thread([this] { ioLoop(); });
    return true;
}

void CommandServer::stop() {
    if (thread_.joinable()) {
        char c = 0;
        (void)!write(stopPipe_[1], &c, 1);
        thread_.join();
    }
    for (int& fd : stopPipe_)
        if (fd >= 0) close(fd), fd = -1;
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
        unlink(path_.c_str());
    }
    if (!infoPath_.empty()) unlink(infoPath_.c_str()), infoPath_.clear();
    std::lock_guard lock(mu_);
    queue_.clear();
}

void CommandServer::ioLoop() {
    std::map<int, std::shared_ptr<Connection>> conns;
    std::vector<pollfd> fds;
    char buf[65536];
    for (;;) {
        fds.clear();
        fds.push_back({stopPipe_[0], POLLIN, 0});
        fds.push_back({listenFd_, POLLIN, 0});
        for (const auto& [fd, c] : conns) fds.push_back({fd, POLLIN, 0});
        if (::poll(fds.data(), nfds_t(fds.size()), -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents) break;
        if (fds[1].revents & POLLIN) {
            int fd = accept(listenFd_, nullptr, nullptr);
            if (fd >= 0) {
                auto c = std::make_shared<Connection>();
                c->fd = fd;
                conns[fd] = std::move(c);
            }
        }
        bool queued = false;
        for (size_t i = 2; i < fds.size(); ++i) {
            if (!fds[i].revents) continue;
            auto it = conns.find(fds[i].fd);
            if (it == conns.end()) continue;
            std::shared_ptr<Connection> c = it->second;
            ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                // Stop reading. Pending replies keep the socket open until they are sent.
                conns.erase(it);
                continue;
            }
            c->buffer.append(buf, size_t(n));
            size_t start = 0, nl;
            while ((nl = c->buffer.find('\n', start)) != std::string::npos) {
                std::string line = c->buffer.substr(start, nl - start);
                start = nl + 1;
                if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
                std::lock_guard lock(mu_);
                queue_.emplace_back(c, std::move(line));
                queued = true;
            }
            c->buffer.erase(0, start);
        }
        if (queued && wake_) wake_();
    }
}

size_t CommandServer::poll() {
    std::vector<std::pair<std::shared_ptr<Connection>, std::string>> work;
    {
        std::lock_guard lock(mu_);
        work.swap(queue_);
    }
    for (auto& [conn, line] : work) {
        json request = json::parse(line, nullptr, false);
        if (request.is_discarded()) {
            conn->send({{"ok", false}, {"error", "request is not valid JSON"}});
            continue;
        }
        commands_.run(request, [conn](json reply) { conn->send(reply); });
    }
    return work.size();
}

}  // namespace mf
