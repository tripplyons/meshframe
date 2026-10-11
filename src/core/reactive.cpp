#include "core/reactive.hpp"

#include <algorithm>
#include <stdexcept>

namespace mf {

Graph::Index Graph::addSource(std::string key, Value initial, std::string label) {
    if (byKey_.contains(key)) throw std::runtime_error("duplicate graph node: " + key);
    Index i = Index(nodes_.size());
    Node n;
    n.label = label.empty() ? key : std::move(label);
    n.key = std::move(key);
    n.kind = Kind::Source;
    n.value = std::move(initial);
    n.state = Node::State::Clean;
    byKey_.emplace(n.key, i);
    nodes_.push_back(std::move(n));
    return i;
}

Graph::Index Graph::addComputed(std::string key, Kind kind, Compute fn, std::string label, uint64_t subject) {
    if (byKey_.contains(key)) throw std::runtime_error("duplicate graph node: " + key);
    Index i = Index(nodes_.size());
    Node n;
    n.label = label.empty() ? key : std::move(label);
    n.key = std::move(key);
    n.kind = kind;
    n.compute = std::move(fn);
    n.subject = subject;
    byKey_.emplace(n.key, i);
    nodes_.push_back(std::move(n));
    return i;
}

Graph::Index Graph::find(std::string_view key) const {
    auto it = byKey_.find(std::string(key));
    return it == byKey_.end() ? kNone : it->second;
}

void Graph::clear() {
    nodes_.clear();
    byKey_.clear();
    stack_.clear();
}

bool Graph::set(Index i, Value v) {
    Node& n = nodes_[i];
    if (n.value == v) return false;
    n.value = std::move(v);
    ++n.evalCount;
    markDependents(i);
    return true;
}

void Graph::markDependents(Index i) {
    // Direct dependents are dirty; their dependents only need a check.
    std::vector<Index> work;
    for (Index d : nodes_[i].dependents) {
        nodes_[d].state = Node::State::Dirty;
        work.push_back(d);
    }
    while (!work.empty()) {
        Index cur = work.back();
        work.pop_back();
        for (Index d : nodes_[cur].dependents) {
            if (nodes_[d].state == Node::State::Clean) {
                nodes_[d].state = Node::State::Check;
                work.push_back(d);
            }
        }
    }
}

void Graph::addEdge(Index from, Index to) {
    // `from` depends on `to`.
    auto& deps = nodes_[from].deps;
    if (std::find(deps.begin(), deps.end(), to) != deps.end()) return;
    deps.push_back(to);
    nodes_[to].dependents.push_back(from);
}

void Graph::track(Index node) {
    if (!stack_.empty()) addEdge(stack_.back(), node);
}

void Graph::reportError(std::string message) {
    if (stack_.empty()) return;
    Node& n = nodes_[stack_.back()];
    if (n.error.empty()) n.error = std::move(message);
}

const Value& Graph::get(Index i) {
    if (!stack_.empty()) addEdge(stack_.back(), i);
    if (nodes_[i].computing) {
        // A cycle: report it on every node in the loop and return the stale value.
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
            nodes_[*it].error = "dependency cycle through " + nodes_[i].key;
            if (*it == i) break;
        }
        return nodes_[i].value;
    }
    update(i);
    return nodes_[i].value;
}

void Graph::update(Index i) {
    Node& n = nodes_[i];
    if (n.kind == Kind::Source || n.state == Node::State::Clean) return;
    if (n.state == Node::State::Check) {
        // Copy: refreshing a dependency can rewire edges.
        auto deps = n.deps;
        for (Index d : deps) {
            update(d);
            if (nodes_[i].state == Node::State::Dirty) break;
        }
    }
    if (nodes_[i].state == Node::State::Dirty) recompute(i);
    nodes_[i].state = Node::State::Clean;
}

void Graph::recompute(Index i) {
    // Drop old edges; the computation records fresh ones.
    for (Index d : nodes_[i].deps) {
        auto& back = nodes_[d].dependents;
        back.erase(std::remove(back.begin(), back.end(), i), back.end());
    }
    nodes_[i].deps.clear();
    nodes_[i].error.clear();
    nodes_[i].computing = true;
    stack_.push_back(i);
    Value result;
    try {
        result = nodes_[i].compute(*this);
    } catch (const std::exception& e) {
        nodes_[i].error = e.what();
    }
    stack_.pop_back();
    Node& n = nodes_[i];
    n.computing = false;
    ++n.evalCount;
    n.state = Node::State::Clean;
    if (!(n.value == result)) {
        n.value = std::move(result);
        for (Index d : n.dependents) nodes_[d].state = Node::State::Dirty;
    }
}

const char* kindName(Graph::Kind k) {
    switch (k) {
        case Graph::Kind::Source: return "source";
        case Graph::Kind::Signal: return "signal";
        case Graph::Kind::Property: return "property";
        case Graph::Kind::Clip: return "clip";
        case Graph::Kind::Transition: return "transition";
        case Graph::Kind::Output: return "output";
    }
    return "?";
}

}  // namespace mf
