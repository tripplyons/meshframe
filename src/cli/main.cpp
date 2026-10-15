// meshframe-cli: runs editor commands in a running meshframe session over its
// Unix socket, or headless on a project file.

#include "session/commands.hpp"
#include "session/server.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using namespace mf;

constexpr const char* kUsage = R"(usage: meshframe-cli [options] COMMAND [ARGS...]
       meshframe-cli [options] -
       meshframe-cli sessions
       meshframe-cli [--idle-timeout SEC] serve FILE

Runs a command in a running meshframe session, or headless on a project file.
ARGS are positional values or name=value pairs. Run "help" for the command
list and "help COMMAND" for one command's arguments.

With "-", reads one request per line from stdin: words as on the command line,
or a JSON object {"cmd": ..., "args": {...}}. Prints one JSON reply per line.
With "--batch -", sends all stdin lines as one batch: one undo step, and no
changes if any command fails. Prints one JSON reply.

"serve FILE" starts a headless session on FILE in the background and prints
its pid and socket once it is ready. It saves FILE after each change, so
separate calls share one undo history. Stop it with the "quit" command; it
also stops after --idle-timeout seconds without requests (default 1800, 0
never). While a session has FILE open, "--project FILE" sends commands to it.

options:
  --session PID     use the session of this meshframe process
  --socket PATH     use the session listening on this socket
  --project FILE    run on FILE: in the running session that has it open, or
                    else headless (created if missing), saving FILE after
                    changes. GUI commands (ui.*) need a running editor.
  --no-save         with --project, do not save
  --json            print the full JSON reply
  --batch           with "-", run stdin as one batch
  --idle-timeout SEC  with serve, stop after SEC seconds without requests
  --timeout SEC     seconds to wait for a reply (default 120, 0 waits forever)
  -h, --help        show this help

exit status: 0 on success, 1 when a command fails, 2 for usage or connection errors.
)";

struct Options {
    std::optional<int> session;
    std::string socket, project;
    bool noSave = false, rawJson = false, stdinMode = false, batch = false;
    double timeout = 120, idleTimeout = 1800;
    std::vector<std::string> words;
};

[[noreturn]] void usageError(const std::string& msg) {
    fmt::print(stderr, "meshframe-cli: {}\n(see meshframe-cli --help)\n", msg);
    std::exit(2);
}

Options parseOptions(int argc, char** argv) {
    Options o;
    int i = 1;
    auto value = [&](const char* name) -> std::string {
        if (i + 1 >= argc) usageError(fmt::format("{} needs a value", name));
        return argv[++i];
    };
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            fmt::print("{}", kUsage);
            std::exit(0);
        } else if (a == "--session") {
            std::string v = value("--session");
            try {
                o.session = std::stoi(v);
            } catch (...) {
                usageError("--session needs a process id");
            }
        } else if (a == "--socket") o.socket = value("--socket");
        else if (a == "--project") o.project = value("--project");
        else if (a == "--no-save") o.noSave = true;
        else if (a == "--json") o.rawJson = true;
        else if (a == "--batch") o.batch = true;
        else if (a == "--timeout") {
            try {
                o.timeout = std::stod(value("--timeout"));
            } catch (...) {
                usageError("--timeout needs a number of seconds");
            }
        } else if (a == "--idle-timeout") {
            try {
                o.idleTimeout = std::stod(value("--idle-timeout"));
            } catch (...) {
                usageError("--idle-timeout needs a number of seconds");
            }
        } else if (a == "-") {
            o.stdinMode = true;
        } else if (a.starts_with("--")) usageError("unknown option " + a);
        else break;
    }
    for (; i < argc; ++i) o.words.push_back(argv[i]);
    if (o.stdinMode && !o.words.empty()) usageError("\"-\" reads commands from stdin; do not also pass a command");
    if (!o.stdinMode && o.words.empty()) usageError("no command");
    if (o.batch && !o.stdinMode) usageError("--batch reads commands from stdin; pass \"-\"");
    if (!o.project.empty() && (o.session || !o.socket.empty())) usageError("--project runs headless; do not combine it with --session or --socket");
    return o;
}

// A request from a stdin line: a JSON object, or words.
json requestFromLine(const std::string& line, const std::string& cwd) {
    size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] == '{') {
        json r = json::parse(line, nullptr, false);
        if (r.is_discarded()) throw std::runtime_error("request is not valid JSON");
        if (r.is_object() && !r.contains("cwd")) r["cwd"] = cwd;
        return r;
    }
    return Commands::requestFromWords(Commands::splitWords(line), cwd);
}

// Prints a reply as the user asked. Returns false when the command failed.
bool printReply(const json& reply, bool raw) {
    bool ok = reply.value("ok", false);
    if (raw) {
        fmt::print("{}\n", reply.dump());
    } else if (!ok) {
        fmt::print(stderr, "error: {}\n", reply.value("error", std::string("unknown error")));
    } else {
        const json& r = reply.contains("result") ? reply["result"] : json();
        if (r.is_string()) fmt::print("{}\n", r.get<std::string>());
        else if (!r.is_null()) fmt::print("{}\n", r.dump(2));
    }
    std::fflush(stdout);
    return ok;
}

// ---- socket client ------------------------------------------------------------------

class Client {
public:
    ~Client() {
        if (fd_ >= 0) close(fd_);
    }

    bool connect(const std::string& path, std::string* err) {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) return *err = "socket path is too long", false;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0 || ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            *err = fmt::format("cannot connect to {}: {}", path, std::strerror(errno));
            return false;
        }
        return true;
    }

    // Sends one request and waits for its reply line.
    std::optional<json> call(const json& request, double timeout, std::string* err) {
        std::string line = request.dump() + "\n";
        for (size_t sent = 0; sent < line.size();) {
            ssize_t n = send(fd_, line.data() + sent, line.size() - sent, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return *err = fmt::format("the session closed the connection: {}", std::strerror(errno)), std::nullopt;
            sent += size_t(n);
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
        for (;;) {
            if (size_t nl = buffer_.find('\n'); nl != std::string::npos) {
                std::string reply = buffer_.substr(0, nl);
                buffer_.erase(0, nl + 1);
                json j = json::parse(reply, nullptr, false);
                if (j.is_discarded()) return *err = "the session sent a reply that is not JSON", std::nullopt;
                return j;
            }
            int wait = -1;
            if (timeout > 0) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) return *err = fmt::format("no reply within {} s", timeout), std::nullopt;
                wait = int(left);
            }
            pollfd p{fd_, POLLIN, 0};
            int r = ::poll(&p, 1, wait);
            if (r < 0 && errno == EINTR) continue;
            if (r == 0) continue;
            char buf[65536];
            ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return *err = "the session closed the connection", std::nullopt;
            buffer_.append(buf, size_t(n));
        }
    }

private:
    int fd_ = -1;
    std::string buffer_;
};

std::string findSocket(const Options& o) {
    if (!o.socket.empty()) return o.socket;
    std::vector<SessionInfo> sessions = listSessions();
    if (o.session) {
        for (const auto& s : sessions)
            if (s.pid == *o.session) return s.socket;
        usageError(fmt::format("no meshframe session with process id {} (run \"meshframe-cli sessions\")", *o.session));
    }
    if (sessions.empty()) usageError("no running meshframe session; start meshframe, or use --project FILE to work headless");
    if (sessions.size() > 1) {
        std::string pids;
        for (const auto& s : sessions) pids += fmt::format(" {}", s.pid);
        usageError(fmt::format("several sessions are running (process ids:{}); choose one with --session PID", pids));
    }
    return sessions[0].socket;
}

int listSessionsCommand(const Options& o) {
    std::vector<SessionInfo> sessions = listSessions();
    json out = json::array();
    for (const auto& s : sessions) {
        json entry = {{"pid", s.pid}, {"kind", s.kind}, {"socket", s.socket}};
        Client c;
        std::string err;
        if (!c.connect(s.socket, &err)) entry["error"] = err;
        else if (auto reply = c.call({{"cmd", "status"}}, 5, &err); reply && reply->value("ok", false)) {
            const json& st = (*reply)["result"];
            for (const char* key : {"path", "name", "dirty", "time", "playing"})
                if (st.contains(key)) entry[key] = st[key];
        } else entry["error"] = reply ? reply->value("error", std::string()) : err;
        out.push_back(std::move(entry));
    }
    if (!o.rawJson && out.empty()) fmt::print("No running sessions.\n");
    else fmt::print("{}\n", out.dump(2));
    return 0;
}

// Runs requests one at a time through `call`. Returns the exit status.
int runRequests(const Options& o, const std::function<std::optional<json>(const json&, std::string*)>& call) {
    std::string cwd = std::filesystem::current_path().string();
    if (!o.stdinMode) {
        std::string err;
        auto reply = call(Commands::requestFromWords(o.words, cwd), &err);
        if (!reply) usageError(err);
        return printReply(*reply, o.rawJson) ? 0 : 1;
    }
    int status = 0;
    std::string line;
    json batch = json::array();
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos || line.starts_with("#")) continue;
        if (o.batch) {
            try {
                batch.push_back(requestFromLine(line, cwd));
            } catch (const std::exception& e) {
                usageError(fmt::format("line \"{}\": {}", line, e.what()));
            }
            continue;
        }
        json reply;
        try {
            std::string err;
            auto r = call(requestFromLine(line, cwd), &err);
            if (!r) usageError(err);
            reply = *r;
        } catch (const std::exception& e) {
            reply = {{"ok", false}, {"error", e.what()}};
        }
        if (!reply.value("ok", false)) status = 1;
        printReply(reply, true);
    }
    if (o.batch) {
        std::string err;
        auto reply = call({{"cmd", "batch"}, {"args", {{"commands", batch}}}, {"cwd", cwd}}, &err);
        if (!reply) usageError(err);
        status = printReply(*reply, true) ? 0 : 1;
    }
    return status;
}

// The running session whose project file is `path`, if any.
std::optional<SessionInfo> sessionWithFile(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path want = fs::weakly_canonical(fs::absolute(path), ec);
    for (const auto& s : listSessions()) {
        Client c;
        std::string err;
        if (!c.connect(s.socket, &err)) continue;
        auto reply = c.call({{"cmd", "status"}}, 5, &err);
        if (!reply || !reply->value("ok", false)) continue;
        std::string open = (*reply)["result"].value("path", "");
        if (!open.empty() && fs::weakly_canonical(open, ec) == want) return s;
    }
    return std::nullopt;
}

int runRemoteOn(const Options& o, const std::string& socket) {
    Client client;
    std::string err;
    if (!client.connect(socket, &err)) usageError(err);
    return runRequests(o, [&](const json& request, std::string* e) { return client.call(request, o.timeout, e); });
}

int runHeadless(const Options& o) {
    // Another process writing the same file would lose one side's edits.
    if (!o.noSave)
        if (auto s = sessionWithFile(o.project)) return runRemoteOn(o, s->socket);
    Session session;
    session.setSource("cli");
    Commands commands(session);
    std::string err;
    namespace fs = std::filesystem;
    if (fs::exists(o.project)) {
        if (!session.open(o.project, &err)) usageError(err);
    }
    int status = runRequests(o, [&](const json& request, std::string*) { return std::optional<json>(commands.runSync(request)); });
    if (!o.noSave && session.dirty()) {
        std::string path = session.path().empty() ? o.project : session.path();
        if (!session.save(path, &err)) {
            fmt::print(stderr, "meshframe-cli: cannot save {}: {}\n", path, err);
            return 2;
        }
    }
    session.exportJob().wait();
    return status;
}

int runRemote(const Options& o) { return runRemoteOn(o, findSocket(o)); }

// Runs a headless session on `file` until `quit` or the idle timeout. Writes
// one JSON line to `ready` once it serves requests, or an error.
int serveLoop(const std::string& file, double idleTimeout, int ready) {
    auto report = [&](const json& j) {
        std::string line = j.dump() + "\n";
        (void)!write(ready, line.data(), line.size());
        close(ready);
    };
    Session session;
    session.setSource("cli");
    Commands commands(session);
    std::string err;
    bool opened = std::filesystem::exists(file) ? session.open(file, &err) : session.save(file, &err);
    if (!opened) return report({{"ok", false}, {"error", err}}), 2;

    bool quit = false;
    commands.add({"quit", "Save and stop this headless session", {}, [&](const json&, CommandContext&) -> json {
                      quit = true;
                      return nullptr;
                  }});
    std::mutex mu;
    std::condition_variable cv;
    bool woken = false;
    CommandServer server(commands, [&] {
        std::lock_guard lock(mu);
        woken = true;
        cv.notify_one();
    }, "headless");
    if (!server.startDefault(&err)) return report({{"ok", false}, {"error", err}}), 2;
    report({{"ok", true}, {"pid", getpid()}, {"socket", server.path()}, {"path", session.path()}});

    using Clock = std::chrono::steady_clock;
    auto lastRequest = Clock::now();
    while (!quit) {
        // Deferred replies and exports need ticks; otherwise sleep until a request.
        bool busy = !session.pollers.empty() || session.exportJob().running();
        {
            std::unique_lock lock(mu);
            cv.wait_for(lock, busy ? std::chrono::milliseconds(5) : std::chrono::milliseconds(500), [&] { return woken; });
            woken = false;
        }
        if (server.poll() > 0) lastRequest = Clock::now();
        session.tick();
        if (session.dirty() && !session.save(session.path(), &err)) session.log(LogLine::Level::Error, "cannot save: " + err);
        bool idle = idleTimeout > 0 && Clock::now() - lastRequest > std::chrono::duration<double>(idleTimeout);
        if (idle && session.pollers.empty() && !session.exportJob().running()) break;
    }
    server.stop();
    session.exportJob().wait();
    if (session.dirty()) session.save(session.path(), &err);
    return 0;
}

int serveCommand(const Options& o) {
    if (o.words.size() != 2) usageError("usage: meshframe-cli serve FILE");
    std::string file = std::filesystem::absolute(o.words[1]).lexically_normal().string();
    if (auto s = sessionWithFile(file)) {
        fmt::print("{}\n", json{{"pid", s->pid}, {"kind", s->kind}, {"socket", s->socket}, {"path", file}, {"running", true}}.dump());
        return 0;
    }
    int fds[2];
    if (pipe(fds) != 0) usageError(fmt::format("pipe: {}", std::strerror(errno)));
    pid_t pid = fork();
    if (pid < 0) usageError(fmt::format("fork: {}", std::strerror(errno)));
    if (pid == 0) {
        // The session outlives this command: no terminal, no inherited output.
        close(fds[0]);
        setsid();
        int null = open("/dev/null", O_RDWR);
        for (int fd : {0, 1, 2}) dup2(null, fd);
        if (null > 2) close(null);
        std::exit(serveLoop(file, o.idleTimeout, fds[1]));
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) != 0;) {
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) break;
        out.append(buf, size_t(n));
    }
    close(fds[0]);
    json r = json::parse(out, nullptr, false);
    if (r.is_discarded() || !r.value("ok", false)) {
        std::string err = r.is_discarded() ? "the session exited before it was ready" : r.value("error", std::string("unknown error"));
        fmt::print(stderr, "meshframe-cli: cannot serve {}: {}\n", file, err);
        return 2;
    }
    r.erase("ok");
    fmt::print("{}\n", r.dump());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o = parseOptions(argc, argv);
    if (!o.stdinMode && o.words.size() == 1 && o.words[0] == "sessions" && o.project.empty()) return listSessionsCommand(o);
    if (!o.stdinMode && !o.words.empty() && o.words[0] == "serve" && o.project.empty()) return serveCommand(o);
    return o.project.empty() ? runRemote(o) : runHeadless(o);
}
