#pragma once

#include "core/value.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mf {

// Push-pull reactive graph with dynamic dependency tracking.
//
// Setting a source marks direct dependents dirty and further dependents as
// "check". Reading a node refreshes it only if one of its dependencies actually
// changed, so unchanged values stop propagation. Dependencies are recorded
// while a computed node runs, so they follow the code path that was taken.
class Graph {
public:
    using Index = uint32_t;
    static constexpr Index kNone = UINT32_MAX;

    enum class Kind : uint8_t { Source, Signal, Property, Clip, Transition, Output };
    using Compute = std::function<Value(Graph&)>;

    struct Node {
        std::string key;
        std::string label;
        Kind kind = Kind::Source;
        Compute compute;
        Value value;
        std::string error;
        std::vector<Index> deps;
        std::vector<Index> dependents;
        uint64_t evalCount = 0;
        uint64_t subject = 0;  // id of the model object this node belongs to
        enum class State : uint8_t { Clean, Check, Dirty } state = State::Dirty;
        bool computing = false;
    };

    Index addSource(std::string key, Value initial, std::string label = {});
    Index addComputed(std::string key, Kind kind, Compute fn, std::string label = {}, uint64_t subject = 0);

    // Replaces a source value. Returns true when the value changed.
    bool set(Index node, Value v);
    const Value& get(Index node);
    Index find(std::string_view key) const;

    // Records a dependency of the running computation on `node` without reading it.
    void track(Index node);
    Index current() const { return stack_.empty() ? kNone : stack_.back(); }

    // Attaches an error to the node that is computing.
    void reportError(std::string message);

    const std::vector<Node>& nodes() const { return nodes_; }
    const Node& node(Index i) const { return nodes_[i]; }
    size_t size() const { return nodes_.size(); }
    void clear();

private:
    void update(Index i);
    void recompute(Index i);
    void markDependents(Index i);
    void addEdge(Index from, Index to);

    std::vector<Node> nodes_;
    std::unordered_map<std::string, Index> byKey_;
    std::vector<Index> stack_;
};

const char* kindName(Graph::Kind k);

}  // namespace mf
