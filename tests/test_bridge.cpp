#include "session/server.hpp"
#include "shell_harness.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <future>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>

using namespace mf;
using namespace mf::ui;
namespace fs = std::filesystem;

namespace {

struct CliResult {
    int status = -1;
    std::string out;
};

std::string quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

// Runs meshframe-cli with a shell `stdinFile` redirect when given.
CliResult cli(const std::vector<std::string>& args, const std::string& stdinFile = {}) {
    std::string cmd = quote(std::string(MF_TEST_BIN_DIR) + "/meshframe-cli");
    for (const auto& a : args) cmd += " " + quote(a);
    if (!stdinFile.empty()) cmd += " < " + quote(stdinFile);
    cmd += " 2>&1";
    CliResult r;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) r.out.append(buf, n);
    int st = pclose(p);
    r.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}

// Runs the CLI on another thread while `pump` serves requests on this one.
CliResult cliWhile(const std::vector<std::string>& args, const std::function<void()>& pump, const std::string& stdinFile = {}) {
    auto f = std::async(std::launch::async, [&] { return cli(args, stdinFile); });
    while (f.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) pump();
    return f.get();
}

std::string writeFile(const fs::path& p, const std::string& text) {
    std::ofstream(p) << text;
    return p.string();
}

// A private runtime directory, so tests never see real sessions.
struct RuntimeDir {
    fs::path path = fs::path("/tmp") / ("mf-test-" + std::to_string(getpid()));
    RuntimeDir() {
        fs::remove_all(path);
        fs::create_directories(path);
        setenv("MESHFRAME_RUNTIME_DIR", path.c_str(), 1);
    }
    ~RuntimeDir() {
        unsetenv("MESHFRAME_RUNTIME_DIR");
        fs::remove_all(path);
    }
};

}  // namespace

TEST(Bridge, HeadlessProjectFile) {
    fs::path dir = fixtures::dir() / "bridge";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string file = (dir / "demo.mfproj").string();

    CliResult r = cli({"--project", file, "clip.add", "solid", "V1", "0", "3", "bg"});
    ASSERT_EQ(r.status, 0) << r.out;
    EXPECT_NE(r.out.find("\"kind\": \"solid\""), std::string::npos) << r.out;
    ASSERT_TRUE(fs::exists(file)) << "a changing command saves the new file";

    r = cli({"--project", file, "prop.set", "bg", "color", "#ff0000"});
    EXPECT_EQ(r.status, 0) << r.out;
    r = cli({"--project", file, "prop.get", "bg", "color"});
    EXPECT_NE(r.out.find("#ff0000"), std::string::npos) << r.out;

    r = cli({"--project", file, "--no-save", "clip.remove", "bg"});
    EXPECT_EQ(r.status, 0) << r.out;
    r = cli({"--project", file, "status"});
    EXPECT_NE(r.out.find("\"clips\": 1"), std::string::npos) << "--no-save left the file unchanged: " << r.out;

    r = cli({"--project", file, "clip.add", "bogus"});
    EXPECT_EQ(r.status, 1);
    EXPECT_NE(r.out.find("error: "), std::string::npos) << r.out;
    EXPECT_EQ(cli({"--project", file}).status, 2) << "no command";
    EXPECT_EQ(cli({"--bogus", "status"}).status, 2);

    // Stdin: words and JSON lines, one reply per line, ids echoed, comments skipped.
    std::string input = writeFile(dir / "requests.txt",
                                  "# a comment\n"
                                  "seek 1.5\n"
                                  "{\"cmd\": \"prop.get\", \"args\": {\"target\": \"bg\", \"name\": \"color\"}, \"id\": 7}\n"
                                  "nope\n");
    r = cli({"--project", file, "-"}, input);
    EXPECT_EQ(r.status, 1) << "one request failed";
    std::istringstream lines(r.out);
    std::vector<json> replies;
    for (std::string line; std::getline(lines, line);) replies.push_back(json::parse(line));
    ASSERT_EQ(replies.size(), 3u) << r.out;
    EXPECT_TRUE(replies[0]["ok"].get<bool>());
    EXPECT_EQ(replies[1]["id"], 7);
    EXPECT_EQ(replies[1]["result"]["value"], "#ff0000");
    EXPECT_FALSE(replies[2]["ok"].get<bool>());
}

TEST(Bridge, SocketSessionDrivesTheGui) {
    RuntimeDir runtime;
    ShellHarness h;
    h.frame();
    CommandServer server(h.commands, nullptr);
    std::string err;
    ASSERT_TRUE(server.startDefault(&err)) << err;
    EXPECT_EQ(server.path(), (runtime.path / (std::to_string(getpid()) + ".sock")).string());
    auto pump = [&] {
        server.poll();
        h.frame();
    };

    CliResult r = cliWhile({"sessions"}, pump);
    ASSERT_EQ(r.status, 0) << r.out;
    json sessions = json::parse(r.out);
    ASSERT_EQ(sessions.size(), 1u) << r.out;
    EXPECT_EQ(sessions[0]["pid"], getpid());
    EXPECT_EQ(sessions[0]["socket"], server.path());

    // Without --session, the only running session is used.
    r = cliWhile({"clip.add", "text", "V2", "0", "2", "title"}, pump);
    ASSERT_EQ(r.status, 0) << r.out;
    ASSERT_EQ(h.session.project().clips.size(), 1u);
    EXPECT_EQ(h.session.project().clips[0].name, "title");

    r = cliWhile({"--session", std::to_string(getpid()), "ui.pane", "1", "graph", "space=3"}, pump);
    ASSERT_EQ(r.status, 0) << r.out;
    EXPECT_EQ(h.shell->workspace().space(2).panes[0].view, "graph");

    // A deferred reply: the screenshot is taken after the next frame.
    fs::path shot = fixtures::dir() / "bridge-shot.png";
    fs::remove(shot);
    r = cliWhile({"--json", "ui.screenshot", shot.string()}, pump);
    ASSERT_EQ(r.status, 0) << r.out;
    EXPECT_EQ(json::parse(r.out)["result"]["width"], h.w);
    EXPECT_TRUE(fs::exists(shot));

    // Several requests share one connection; bad JSON gets an error reply.
    std::string input = writeFile(fixtures::dir() / "bridge-requests.txt", "undo\n{\"cmd\": \"status\", \"id\": \"s\"}\n{oops\n");
    r = cliWhile({"-"}, pump, input);
    EXPECT_EQ(r.status, 1) << r.out;
    std::istringstream lines(r.out);
    std::vector<json> replies;
    for (std::string line; std::getline(lines, line);) replies.push_back(json::parse(line));
    ASSERT_EQ(replies.size(), 3u) << r.out;
    EXPECT_EQ(replies[0]["result"], true);
    EXPECT_EQ(replies[1]["id"], "s");
    EXPECT_EQ(replies[1]["result"]["clips"], 0);
    EXPECT_FALSE(replies[2]["ok"].get<bool>());

    r = cliWhile({"clip.get", "missing"}, pump);
    EXPECT_EQ(r.status, 1) << r.out;
    r = cliWhile({"--session", "999999", "status"}, pump);
    EXPECT_EQ(r.status, 2) << r.out;

    server.stop();
    EXPECT_FALSE(fs::exists(server.path()));
    EXPECT_TRUE(fs::is_empty(runtime.path)) << "stop removes the socket and info file";
    r = cli({"status"});
    EXPECT_EQ(r.status, 2);
    EXPECT_NE(r.out.find("no running meshframe session"), std::string::npos) << r.out;
}

TEST(Bridge, SessionsSkipsAndRemovesDeadProcesses) {
    RuntimeDir runtime;
    // A process id that is not running: a child that already exited.
    pid_t child = fork();
    if (child == 0) _exit(0);
    waitpid(child, nullptr, 0);
    std::string info = writeFile(runtime.path / (std::to_string(child) + ".json"),
                                 json{{"pid", child}, {"socket", (runtime.path / "gone.sock").string()}}.dump());
    EXPECT_TRUE(listSessions().empty());
    EXPECT_FALSE(fs::exists(info));
}
