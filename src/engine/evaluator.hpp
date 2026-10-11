#pragma once

#include "core/reactive.hpp"
#include "model/project.hpp"
#include "model/schema.hpp"
#include "script/lua_runtime.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mf {

struct NodeError {
    std::string key;
    std::string message;
    int line = 0;  // script line, when known
};

// Evaluates a project's properties, signals, and transitions over time through
// a reactive graph. Holds the effective project: the user's project plus the
// clips the script declares.
class Evaluator : public LuaRuntime::Host {
public:
    Evaluator();

    // Copies the project, reruns the script if its text changed, and rebuilds the graph.
    void setProject(const Project& p);
    const Project& project() const { return effective_; }
    const ScriptOutput& script() const { return script_; }

    void setTime(double t);
    double time() const { return time_; }

    Value prop(const Clip& c, std::string_view name);
    double num(const Clip& c, std::string_view name, double fallback = 0);
    Color color(const Clip& c, std::string_view name, Color fallback = {});
    std::string str(const Clip& c, std::string_view name);
    Value effectProp(const Effect& e, std::string_view name);
    Value transitionProp(const Transition& tr, std::string_view name);
    // Eased 0..1 mix of a transition at the current time.
    double transitionMix(const Transition& tr);
    Value signal(std::string_view name);

    // Time window of a transition: centered on clip B's start.
    static std::pair<double, double> window(const Project& p, const Transition& tr);
    // Whether a clip contributes at time t, including transition overhang.
    bool clipVisibleAt(const Clip& c, double t) const;

    // Pulls output nodes so the graph shows current state.
    void refreshOutputs();

    Graph& graph() { return graph_; }
    const Graph& graph() const { return graph_; }
    LuaRuntime& lua() { return lua_; }
    Value eval(const std::string& code, std::string* err);

    std::vector<NodeError> errors() const;
    void reportRuntimeError(const std::string& key, std::string message);
    void clearRuntimeError(const std::string& key);

    std::function<void(std::string)> onLog;

    // LuaRuntime::Host
    double hostTime() override;
    double hostLocalTime() override;
    double hostProgress() override;
    Value hostSignal(std::string_view name) override;
    Value hostProp(std::string_view clip, std::string_view prop) override;
    void hostLog(std::string line) override;

private:
    struct Context {
        double start = 0;
        double duration = 1;
    };
    struct PropNode {
        std::string name;
        Graph::Index node;
    };

    void instantiateScriptClips();
    void rebuildGraph();
    Graph::Index addPropertyNode(const std::string& key, uint64_t owner, const std::string& name, const std::string& label,
                                 const Property& prop,
                                 const PropSpec* spec, Context ctx, bool localCurveTime,
                                 Graph::Kind kind = Graph::Kind::Property);
    Value lookup(uint64_t owner, std::string_view name, const PropertyMap& props);

    Project user_;
    Project effective_;
    ScriptOutput script_;
    std::vector<ScriptError> scriptClipErrors_;
    std::string lastScript_;
    bool scriptRan_ = false;
    LuaRuntime lua_;
    Graph graph_;
    Graph::Index timeNode_ = Graph::kNone;
    Graph::Index outVideo_ = Graph::kNone, outAudio_ = Graph::kNone;
    std::unordered_map<uint64_t, std::vector<PropNode>> propNodes_;
    std::unordered_map<Id, Graph::Index> transitionNodes_;
    std::vector<Context> ctx_;
    std::unordered_map<std::string, std::string> runtimeErrors_;
    double time_ = 0;
};

}  // namespace mf
