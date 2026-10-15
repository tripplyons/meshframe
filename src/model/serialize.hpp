#pragma once

#include "model/project.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace mf {

using json = nlohmann::json;

json valueToJson(const Value& v);
Value valueFromJson(const json& j);
json curveToJson(const Curve& c);
Curve curveFromJson(const json& j);
// Constants serialize as plain values; curves as {"curve": [...]}; expressions as {"expr": "..."}.
json propertyToJson(const Property& p);
Property propertyFromJson(const json& j);
json effectToJson(const Effect& e);
json clipToJson(const Clip& c);
json transitionToJson(const Transition& t);
json trackToJson(const Track& t);
json mediaToJson(const MediaSource& m);
json settingsToJson(const ProjectSettings& s);
json markerToJson(const Marker& m);
json transcriptToJson(const Transcript& t);
Transcript transcriptFromJson(const json& j);
ProjectSettings settingsFromJson(const json& j, ProjectSettings base = {});

json projectToJson(const Project& p);
Project projectFromJson(const json& j);

void saveProject(const Project& p, const std::string& path);
Project loadProject(const std::string& path);

}  // namespace mf
