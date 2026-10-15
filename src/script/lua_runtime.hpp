#pragma once

#include "core/value.hpp"
#include "model/project.hpp"
#include "render/draw_list.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace mf {

struct ScriptError {
    int line = 0;
    std::string message;
};

// A clip declared by the project script with mf.clip{...}.
struct ScriptClipSpec {
    std::string track;  // track name, or empty when trackIndex is used
    int trackIndex = -1;
    ClipKind kind = ClipKind::Text;
    std::string name;
    std::string media;
    double start = 0, duration = 1, in = 0, speed = 1;
    PropertyMap props;
};

struct ScriptOutput {
    std::vector<std::string> signals;
    std::vector<std::string> generators;
    std::vector<std::string> synths;
    std::vector<ScriptClipSpec> clips;
    std::optional<ScriptError> error;
};

// Embedded LuaJIT state. One project script runs in a fresh environment each
// time it changes; property expressions compile into that environment, so they
// can call helpers the script defines.
class LuaRuntime {
public:
    // The evaluator implements this to connect reads to the reactive graph.
    struct Host {
        virtual ~Host() = default;
        virtual double hostTime() = 0;
        virtual double hostLocalTime() = 0;
        virtual double hostProgress() = 0;
        virtual Value hostSignal(std::string_view name) = 0;
        virtual Value hostProp(std::string_view clip, std::string_view prop) = 0;
        virtual void hostLog(std::string line) = 0;
    };

    LuaRuntime();
    ~LuaRuntime();
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    void setHost(Host* host) { host_ = host; }
    void setConstants(double fps, int width, int height, int sampleRate);

    // Runs the project script in a new environment. Clears compiled expressions.
    ScriptOutput runScript(const std::string& source);

    // Compiles an expression (or a statement block that returns a value).
    // Results are cached by source text. Returns -1 and sets err on failure.
    int compile(const std::string& expr, std::string* err);
    Value call(int ref, std::string* err);

    bool hasSignal(const std::string& name) const { return signals_.contains(name); }
    Value callSignal(const std::string& name, std::string* err);
    bool hasGenerator(const std::string& name) const { return generators_.contains(name); }
    bool draw(const std::string& generator, DrawList& out, double t, double lt, double progress, int w, int h,
              std::string* err);
    bool hasSynth(const std::string& name) const { return synths_.contains(name); }
    // Fills `frames` stereo samples starting at time t0.
    bool synth(const std::string& name, double t0, double sampleRate, int frames, float* stereo, std::string* err);

    // Runs code in the script environment, for the console and CLI.
    Value eval(const std::string& code, std::string* err);

    lua_State* state() { return L_; }
    Host* host() { return host_; }
    // Internal: used by Lua C functions.
    ScriptOutput* pending() { return pending_; }
    int registerFunction(int stackIndex);

private:
    friend struct LuaRuntimeAccess;
    void installBase();
    void releaseCompiled();
    bool pcall(int nargs, int nresults, std::string* err, double budgetSeconds);

    lua_State* L_ = nullptr;
    Host* host_ = nullptr;
    int baseEnv_ = -2;    // registry ref of the shared base environment
    int scriptEnv_ = -2;  // registry ref of the current script environment
    std::unordered_map<std::string, int> compiled_;
    std::map<std::string, int> signals_;
    std::map<std::string, int> generators_;
    std::map<std::string, int> synths_;
    std::vector<int> scriptRefs_;
    ScriptOutput* pending_ = nullptr;
};

// One documented name in the scripting API, listed by the `schema` command.
struct LuaApiEntry {
    const char* name;
    const char* signature;
    const char* summary;
};

struct LuaApi {
    std::vector<LuaApiEntry> variables;     // read in expressions and scripts
    std::vector<LuaApiEntry> functions;     // global helpers
    std::vector<LuaApiEntry> canvas;        // methods of a generator's g
    std::vector<LuaApiEntry> declarations;  // mf.* functions for the project script
};
const LuaApi& luaApi();

Value luaToValue(lua_State* L, int index);
std::optional<Color> luaToColor(lua_State* L, int index);

}  // namespace mf
