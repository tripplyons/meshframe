#pragma once

#include "model/serialize.hpp"
#include "session/session.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mf {

// Argument types: id (number or name), number, int, bool, string, value (JSON
// if it parses, else a string), json, path (resolved against the client's cwd).
struct ArgSpec {
    std::string name;
    std::string type;
    bool required = false;
    std::string help;
};

using Reply = std::function<void(json)>;

class CommandContext {
public:
    CommandContext(Session& s, Reply reply) : session(s), reply_(std::move(reply)) {}
    Session& session;
    // Takes over the reply; the handler's return value is ignored. Call the
    // returned function once with the result (or throw-free {"error": ...}).
    Reply defer() {
        deferred_ = true;
        return reply_;
    }
    bool deferred() const { return deferred_; }

private:
    Reply reply_;
    bool deferred_ = false;
};

using Handler = std::function<json(const json& args, CommandContext& ctx)>;

struct CommandSpec {
    std::string name;
    std::string summary;
    std::vector<ArgSpec> args;
    Handler fn;
};

// Named commands over a Session. Requests are {"cmd", "args", "argv", "cwd"};
// positional argv strings fill declared args in order. Replies are
// {"ok": true, "result": ...} or {"ok": false, "error": "..."}.
class Commands {
public:
    explicit Commands(Session& s);

    void add(CommandSpec spec);
    const std::map<std::string, CommandSpec>& all() const { return commands_; }

    void run(const json& request, Reply reply);
    // Runs to completion, ticking the session while a reply is deferred.
    json runSync(const json& request);

    // Builds a request from CLI-style words: name, positional values, key=value pairs.
    static json requestFromWords(const std::vector<std::string>& words, const std::string& cwd);

private:
    json normalize(const CommandSpec& spec, const json& request) const;
    void registerBuiltins();

    Session& session_;
    std::map<std::string, CommandSpec> commands_;
};

// Target lookups that accept ids or names. Throw EditError when not found.
Id resolveClip(const Project& p, const json& v);
Id resolveTrack(const Project& p, const json& v);
Id resolveMedia(const Project& p, const json& v);
// A property owner: clip (id or name), effect id, or transition id.
Id resolveTarget(const Project& p, const json& v);

}  // namespace mf
