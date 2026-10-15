#include "session/diff.hpp"

#include "model/serialize.hpp"

#include <fmt/format.h>

namespace mf {

namespace {

// Collections compared by id, with the field that names their objects.
struct Collection {
    const char* key;
    const char* kind;
    const char* nameField;
};
constexpr Collection kCollections[] = {
    {"tracks", "track", "name"},         {"clips", "clip", "name"},       {"media", "media", "name"},
    {"transitions", "transition", "type"}, {"signals", "signal", "name"}, {"markers", "marker", "name"},
    {"masterAudio", "masterEffect", "type"},
};

bool hasIds(const json& a) {
    if (!a.is_array() || a.empty()) return false;
    for (const auto& e : a)
        if (!e.is_object() || !e.contains("id")) return false;
    return true;
}

const json* findId(const json& a, const json& id) {
    for (const auto& e : a)
        if (e["id"] == id) return &e;
    return nullptr;
}

// Adds each differing leaf under `path` to `out`. Objects recurse by key and
// arrays of objects with ids recurse by id; other values compare whole.
void diffValues(const std::string& path, const json& a, const json& b, json& out) {
    if (a == b) return;
    auto join = [&](const std::string& key) { return path.empty() ? key : path + "." + key; };
    if (a.is_object() && b.is_object()) {
        for (const auto& [k, v] : a.items()) diffValues(join(k), v, b.contains(k) ? b[k] : json(), out);
        for (const auto& [k, v] : b.items())
            if (!a.contains(k)) diffValues(join(k), json(), v, out);
        return;
    }
    bool aIds = hasIds(a) || (a.is_array() && a.empty()), bIds = hasIds(b) || (b.is_array() && b.empty());
    if (!a.is_null() && !b.is_null() && aIds && bIds) {
        auto item = [&](const json& id) { return fmt::format("{}[{}]", path, id.dump()); };
        for (const auto& e : a) {
            const json* other = findId(b, e["id"]);
            diffValues(item(e["id"]), e, other ? *other : json(), out);
        }
        for (const auto& e : b)
            if (!findId(a, e["id"])) diffValues(item(e["id"]), json(), e, out);
        return;
    }
    out[path] = {{"old", a}, {"new", b}};
}

// The project as JSON, with each transcript reduced to its size.
json comparable(const Project& p) {
    json j = projectToJson(p);
    for (auto& m : j["media"])
        if (m.contains("transcript")) {
            const json& t = m["transcript"];
            m["transcript"] = {{"model", t.value("model", "")},
                               {"segments", t.value("segments", json::array()).size()},
                               {"words", t.value("words", json::array()).size()}};
        }
    return j;
}

json entry(const char* kind, const json& object, const char* nameField) {
    json e = {{"kind", kind}, {"id", object["id"]}};
    if (object.contains(nameField)) e["name"] = object[nameField];
    return e;
}

}  // namespace

json diffProjects(const Project& before, const Project& after) {
    json a = comparable(before), b = comparable(after);
    json out = {{"added", json::array()}, {"removed", json::array()}, {"changed", json::array()}};

    json projectFields = json::object();
    for (const char* key : {"name", "settings", "script"}) diffValues(key, a[key], b[key], projectFields);
    if (!projectFields.empty()) out["changed"].push_back({{"kind", "project"}, {"fields", std::move(projectFields)}});

    for (const auto& c : kCollections) {
        const json &oldList = a[c.key], &newList = b[c.key];
        for (const auto& o : oldList) {
            const json* n = findId(newList, o["id"]);
            if (!n) {
                out["removed"].push_back(entry(c.kind, o, c.nameField));
                continue;
            }
            json fields = json::object();
            for (const auto& [k, v] : o.items())
                if (k != "id") diffValues(k, v, n->contains(k) ? (*n)[k] : json(), fields);
            for (const auto& [k, v] : n->items())
                if (!o.contains(k)) diffValues(k, json(), v, fields);
            if (fields.empty()) continue;
            json e = entry(c.kind, *n, c.nameField);
            e["fields"] = std::move(fields);
            out["changed"].push_back(std::move(e));
        }
        for (const auto& n : newList)
            if (!findId(oldList, n["id"])) out["added"].push_back(entry(c.kind, n, c.nameField));
    }
    return out;
}

}  // namespace mf
