// meshframe-cli: runs editor commands in a running meshframe session over its
// Unix socket, or headless on a project file.

#include "session/commands.hpp"
#include "session/server.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <chrono>
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

Runs a command in a running meshframe session, or headless on a project file.
ARGS are positional values or name=value pairs. Run "help" for the command
list and "help COMMAND" for one command's arguments.

With "-", reads one request per line from stdin: words as on the command line,
or a JSON object {"cmd": ..., "args": {...}}. Prints one JSON reply per line.

options:
  --session PID     use the session of this meshframe process
  --socket PATH     use the session listening on this socket
  --project FILE    run headless on FILE (created if missing); saves FILE after
                    changes. GUI commands (ui.*) need a running session.
  --no-save         with --project, do not save
  --json            print the full JSON reply
  --timeout SEC     seconds to wait for a reply (default 120, 0 waits forever)
  -h, --help        show this help

exit status: 0 on success, 1 when a command fails, 2 for usage or connection errors.
)";

struct Options {
    std::optional<int> session;
    std::string socket, project;
    bool noSave = false, rawJson = false, stdinMode = false;
    double timeout = 120;
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
        else if (a == "--timeout") {
            try {
                o.timeout = std::stod(value("--timeout"));
            } catch (...) {
                usageError("--timeout needs a number of seconds");
            }
        } else if (a == "-") {
            o.stdinMode = true;
        } else if (a.starts_with("--")) usageError("unknown option " + a);
        else break;
    }
    for (; i < argc; ++i) o.words.push_back(argv[i]);
    if (o.stdinMode && !o.words.empty()) usageError("\"-\" reads commands from stdin; do not also pass a command");
    if (!o.stdinMode && o.words.empty()) usageError("no command");
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
        json entry = {{"pid", s.pid}, {"socket", s.socket}};
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
    while (std::getline(std::cin, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos || line.starts_with("#")) continue;
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
    return status;
}

int runHeadless(const Options& o) {
    Session session;
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

int runRemote(const Options& o) {
    Client client;
    std::string err;
    if (!client.connect(findSocket(o), &err)) usageError(err);
    return runRequests(o, [&](const json& request, std::string* e) { return client.call(request, o.timeout, e); });
}

}  // namespace

int main(int argc, char** argv) {
    Options o = parseOptions(argc, argv);
    if (!o.stdinMode && o.words.size() == 1 && o.words[0] == "sessions" && o.project.empty()) return listSessionsCommand(o);
    return o.project.empty() ? runRemote(o) : runHeadless(o);
}
