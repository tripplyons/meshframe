#include "script/lua_runtime.hpp"

#include "core/curve.hpp"

#include <fmt/format.h>
#include <lua.hpp>

#include <chrono>
#include <cmath>
#include <cstring>

namespace mf {

namespace {

thread_local std::chrono::steady_clock::time_point g_deadline;

void budgetHook(lua_State* L, lua_Debug*) {
    if (std::chrono::steady_clock::now() > g_deadline) luaL_error(L, "time budget exceeded (infinite loop?)");
}

LuaRuntime* runtimeOf(lua_State* L) {
    return static_cast<LuaRuntime*>(lua_touserdata(L, lua_upvalueindex(1)));
}

void pushValue(lua_State* L, const Value& v) {
    switch (typeOf(v)) {
        case ValueType::None: lua_pushnil(L); break;
        case ValueType::Number: lua_pushnumber(L, std::get<double>(v)); break;
        case ValueType::String: {
            const auto& s = std::get<std::string>(v);
            lua_pushlstring(L, s.data(), s.size());
            break;
        }
        case ValueType::Color: {
            const Color& c = std::get<Color>(v);
            lua_createtable(L, 0, 4);
            lua_pushnumber(L, c.r); lua_setfield(L, -2, "r");
            lua_pushnumber(L, c.g); lua_setfield(L, -2, "g");
            lua_pushnumber(L, c.b); lua_setfield(L, -2, "b");
            lua_pushnumber(L, c.a); lua_setfield(L, -2, "a");
            break;
        }
    }
}

// __index for the base environment: dynamic, tracked reads.
int envIndex(lua_State* L) {
    LuaRuntime* rt = runtimeOf(L);
    size_t len = 0;
    const char* key = lua_tolstring(L, 2, &len);
    if (!key || !rt->host()) {
        lua_pushnil(L);
        return 1;
    }
    std::string_view k(key, len);
    if (k == "t") lua_pushnumber(L, rt->host()->hostTime());
    else if (k == "lt") lua_pushnumber(L, rt->host()->hostLocalTime());
    else if (k == "progress") lua_pushnumber(L, rt->host()->hostProgress());
    else lua_pushnil(L);
    return 1;
}

int sigIndex(lua_State* L) {
    LuaRuntime* rt = runtimeOf(L);
    size_t len = 0;
    const char* key = luaL_checklstring(L, 2, &len);
    if (!rt->host()) return luaL_error(L, "no evaluation context");
    pushValue(L, rt->host()->hostSignal(std::string_view(key, len)));
    return 1;
}

int propFn(lua_State* L) {
    LuaRuntime* rt = runtimeOf(L);
    size_t a = 0, b = 0;
    const char* clip = luaL_checklstring(L, 1, &a);
    const char* prop = luaL_checklstring(L, 2, &b);
    if (!rt->host()) return luaL_error(L, "no evaluation context");
    pushValue(L, rt->host()->hostProp(std::string_view(clip, a), std::string_view(prop, b)));
    return 1;
}

std::string joinArgs(lua_State* L, int first) {
    std::string out;
    int n = lua_gettop(L);
    lua_getglobal(L, "tostring");
    for (int i = first; i <= n; ++i) {
        lua_pushvalue(L, -1);
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        if (i > first) out += '\t';
        if (s) out.append(s, len);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return out;
}

int printFn(lua_State* L) {
    LuaRuntime* rt = runtimeOf(L);
    if (rt->host()) rt->host()->hostLog(joinArgs(L, 1));
    return 0;
}

int easeFn(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    double x = luaL_checknumber(L, 2);
    const EasePreset* p = findEase(name);
    if (!p) return luaL_error(L, "unknown ease '%s'", name);
    lua_pushnumber(L, cubicBezier(x, p->x1, p->y1, p->x2, p->y2));
    return 1;
}

ScriptOutput& pendingOr(lua_State* L, LuaRuntime* rt) {
    if (!rt->pending()) luaL_error(L, "mf.* declarations only work while the project script runs");
    return *rt->pending();
}

int mfSignal(lua_State* L);
int mfGenerator(lua_State* L);
int mfSynth(lua_State* L);
int mfClip(lua_State* L);

// Drawing methods for generator canvases.
DrawList* drawListOf(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__dl");
    auto* dl = static_cast<DrawList*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!dl) luaL_error(L, "canvas used outside its generator call");
    return dl;
}

Color colorArg(lua_State* L, int index, Color fallback = Color::gray(1.0f)) {
    if (lua_isnoneornil(L, index)) return fallback;
    auto c = luaToColor(L, index);
    if (!c) luaL_error(L, "argument %d is not a color", index);
    return *c;
}

float num(lua_State* L, int i) { return float(luaL_checknumber(L, i)); }

int gClear(lua_State* L) {
    DrawCmd c{DrawCmd::Type::Clear};
    c.color = colorArg(L, 2, Color::gray(0.0f));
    drawListOf(L)->cmds.push_back(std::move(c));
    return 0;
}
int gRect(lua_State* L) {
    DrawCmd c{DrawCmd::Type::Rect};
    c.a = num(L, 2); c.b = num(L, 3); c.c = num(L, 4); c.d = num(L, 5);
    c.color = colorArg(L, 6);
    drawListOf(L)->cmds.push_back(std::move(c));
    return 0;
}
int gCircle(lua_State* L) {
    DrawCmd c{DrawCmd::Type::Circle};
    c.a = num(L, 2); c.b = num(L, 3); c.c = num(L, 4);
    c.color = colorArg(L, 5);
    drawListOf(L)->cmds.push_back(std::move(c));
    return 0;
}
int gLine(lua_State* L) {
    DrawCmd c{DrawCmd::Type::Line};
    c.a = num(L, 2); c.b = num(L, 3); c.c = num(L, 4); c.d = num(L, 5);
    c.e = lua_isnoneornil(L, 6) ? 2.0f : num(L, 6);
    c.color = colorArg(L, 7);
    drawListOf(L)->cmds.push_back(std::move(c));
    return 0;
}
int gText(lua_State* L) {
    DrawCmd c{DrawCmd::Type::Text};
    c.text = luaL_checkstring(L, 2);
    c.a = num(L, 3); c.b = num(L, 4);
    c.c = lua_isnoneornil(L, 5) ? 48.0f : num(L, 5);
    c.color = colorArg(L, 6);
    drawListOf(L)->cmds.push_back(std::move(c));
    return 0;
}
int gPoly(lua_State* L) {
    DrawCmd c{DrawCmd::Type::Poly};
    luaL_checktype(L, 2, LUA_TTABLE);
    int n = int(lua_objlen(L, 2));
    if (n < 6 || n % 2) return luaL_error(L, "poly needs a flat list of at least 3 x,y pairs");
    c.points.reserve(n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        c.points.push_back(float(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    c.color = colorArg(L, 3);
    drawListOf(L)->cmds.push_back(std::move(c));
    return 0;
}

const char* kPrelude = R"lua(
local m = math
abs, ceil, floor, sqrt, exp, log = m.abs, m.ceil, m.floor, m.sqrt, m.exp, m.log
sin, cos, tan, asin, acos, atan, atan2 = m.sin, m.cos, m.tan, m.asin, m.acos, m.atan, m.atan2
min, max, fmod, pow, pi, huge = m.min, m.max, m.fmod, m.pow, m.pi, m.huge
function clamp(x, a, b) a = a or 0; b = b or 1; if x < a then return a elseif x > b then return b end return x end
function lerp(a, b, x) return a + (b - a) * x end
function smoothstep(a, b, x) x = clamp((x - a) / (b - a)); return x * x * (3 - 2 * x) end
function round(x) return floor(x + 0.5) end
function rgb(r, g, b, a) return {r = r, g = g, b = b, a = a or 1} end
function gray(v, a) return {r = v, g = v, b = v, a = a or 1} end
function hsv(h, s, v, a)
  h = (h % 1) * 6; local i = floor(h); local f = h - i
  local p, q, u = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
  local r, g, b
  if i == 0 then r, g, b = v, u, p elseif i == 1 then r, g, b = q, v, p
  elseif i == 2 then r, g, b = p, v, u elseif i == 3 then r, g, b = p, q, v
  elseif i == 4 then r, g, b = u, p, v else r, g, b = v, p, q end
  return {r = r, g = g, b = b, a = a or 1}
end
function hash(n)
  local x = sin(n * 12.9898 + 78.233) * 43758.5453
  return x - floor(x)
end
function noise(x)
  local i = floor(x); local f = x - i
  local u = f * f * (3 - 2 * f)
  return lerp(hash(i), hash(i + 1), u) * 2 - 1
end
function osc(freq, phase) return sin((t * freq + (phase or 0)) * 2 * pi) end
)lua";

}  // namespace

const LuaApi& luaApi() {
    static const LuaApi api = {
        .variables =
            {
                {"t", "t", "Project time in seconds"},
                {"lt", "lt", "Seconds since the clip's start"},
                {"progress", "progress", "0 to 1 across the clip, or across a transition's window"},
                {"sig", "sig.NAME", "A project or script signal"},
                {"marker", "marker.NAME", "A timeline marker's time in seconds"},
                {"fps", "fps", "Project frame rate"},
                {"w", "w", "Project width in pixels"},
                {"h", "h", "Project height in pixels"},
                {"sample_rate", "sample_rate", "Project audio sample rate"},
                {"pi", "pi", "The constant pi"},
                {"huge", "huge", "Positive infinity"},
            },
        .functions =
            {
                {"prop", "prop(clip, name)", "Another clip's evaluated property; clip is a name"},
                {"ease", "ease(name, x)", "Applies an ease preset to x in 0..1"},
                {"clamp", "clamp(x, a=0, b=1)", "Limits x to a..b"},
                {"lerp", "lerp(a, b, x)", "Linear blend from a to b"},
                {"smoothstep", "smoothstep(a, b, x)", "Smooth 0..1 step as x goes from a to b"},
                {"round", "round(x)", "Nearest integer"},
                {"hash", "hash(n)", "Pseudo-random 0..1 from a number"},
                {"noise", "noise(x)", "Smooth value noise, -1..1"},
                {"osc", "osc(freq, phase=0)", "Sine of t at freq Hz, -1..1"},
                {"rgb", "rgb(r, g, b, a=1)", "Color from components 0..1"},
                {"gray", "gray(v, a=1)", "Gray color"},
                {"hsv", "hsv(h, s, v, a=1)", "Color from hue, saturation, value, all 0..1"},
                {"print", "print(...)", "Writes a log line"},
                {"abs", "abs(x)", "math.abs"},
                {"ceil", "ceil(x)", "math.ceil"},
                {"floor", "floor(x)", "math.floor"},
                {"sqrt", "sqrt(x)", "math.sqrt"},
                {"exp", "exp(x)", "math.exp"},
                {"log", "log(x)", "math.log"},
                {"sin", "sin(x)", "math.sin"},
                {"cos", "cos(x)", "math.cos"},
                {"tan", "tan(x)", "math.tan"},
                {"asin", "asin(x)", "math.asin"},
                {"acos", "acos(x)", "math.acos"},
                {"atan", "atan(x)", "math.atan"},
                {"atan2", "atan2(y, x)", "math.atan2"},
                {"min", "min(a, b, ...)", "math.min"},
                {"max", "max(a, b, ...)", "math.max"},
                {"fmod", "fmod(a, b)", "math.fmod"},
                {"pow", "pow(a, b)", "math.pow"},
            },
        .canvas =
            {
                {"clear", "g:clear(color=black)", "Fills the canvas"},
                {"rect", "g:rect(x, y, w, h, color=white)", "Filled rectangle; x, y is the top-left corner"},
                {"circle", "g:circle(x, y, r, color=white)", "Filled circle"},
                {"line", "g:line(x1, y1, x2, y2, width=2, color=white)", "Line segment"},
                {"text", "g:text(str, x, y, size=48, color=white)", "Text; x, y is the top-left corner"},
                {"poly", "g:poly({x1, y1, x2, y2, x3, y3, ...}, color=white)", "Filled polygon"},
            },
        .declarations =
            {
                {"signal", "mf.signal(name, fn)", "Declares a signal computed per frame"},
                {"generator", "mf.generator(name, fn(g))", "Declares a generator; g has w, h, t, lt, progress"},
                {"synth", "mf.synth(name, fn(time))", "Declares a synth that returns one sample in -1..1"},
                {"clip", "mf.clip{track=, kind=, start=, duration=, ...}", "Declares a read-only clip; props may be functions"},
                {"log", "mf.log(...)", "Writes a log line"},
            },
    };
    return api;
}

std::optional<Color> luaToColor(lua_State* L, int index) {
    index = index < 0 ? lua_gettop(L) + index + 1 : index;
    if (lua_type(L, index) == LUA_TSTRING) return parseColor(lua_tostring(L, index));
    if (lua_type(L, index) == LUA_TNUMBER) {
        float v = float(lua_tonumber(L, index));
        return Color{v, v, v, 1};
    }
    if (lua_type(L, index) != LUA_TTABLE) return std::nullopt;
    auto get = [&](const char* name, int idx) -> std::optional<float> {
        lua_getfield(L, index, name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, index, idx);
        }
        std::optional<float> v;
        if (lua_type(L, -1) == LUA_TNUMBER) v = float(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return v;
    };
    auto r = get("r", 1), g = get("g", 2), b = get("b", 3), a = get("a", 4);
    if (!r || !g || !b) return std::nullopt;
    return Color{*r, *g, *b, a.value_or(1.0f)};
}

Value luaToValue(lua_State* L, int index) {
    switch (lua_type(L, index)) {
        case LUA_TNUMBER: return lua_tonumber(L, index);
        case LUA_TBOOLEAN: return lua_toboolean(L, index) ? 1.0 : 0.0;
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, index, &len);
            return std::string(s, len);
        }
        case LUA_TTABLE:
            if (auto c = luaToColor(L, index)) return *c;
            return std::string("<table>");
        default: return std::monostate{};
    }
}

LuaRuntime::LuaRuntime() {
    L_ = luaL_newstate();
    luaL_openlibs(L_);
    installBase();
}

LuaRuntime::~LuaRuntime() {
    if (L_) lua_close(L_);
}

void LuaRuntime::installBase() {
    lua_State* L = L_;
    lua_newtable(L);  // base env
    int env = lua_gettop(L);
    const char* globals[] = {"math", "string", "table", "bit", "pairs", "ipairs", "next", "select", "tonumber",
                             "tostring", "type", "unpack", "pcall", "error", "assert", "setmetatable",
                             "getmetatable", "rawget", "rawset", "rawequal"};
    for (const char* g : globals) {
        lua_getglobal(L, g);
        lua_setfield(L, env, g);
    }
    auto closure = [&](lua_CFunction fn, const char* name, int table) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, table, name);
    };
    closure(printFn, "print", env);
    closure(propFn, "prop", env);
    lua_pushcfunction(L, easeFn);
    lua_setfield(L, env, "ease");

    // sig.<name> reads a signal through the graph.
    lua_newtable(L);
    lua_newtable(L);
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, sigIndex, 1);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setfield(L, env, "sig");

    // mf.* declarations.
    lua_newtable(L);
    int mf = lua_gettop(L);
    closure(mfSignal, "signal", mf);
    closure(mfGenerator, "generator", mf);
    closure(mfSynth, "synth", mf);
    closure(mfClip, "clip", mf);
    closure(printFn, "log", mf);
    lua_setfield(L, env, "mf");

    // Dynamic t / lt / progress.
    lua_newtable(L);
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, envIndex, 1);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, env);

    // Drawing methods shared by generator canvases.
    lua_newtable(L);
    lua_newtable(L);
    const std::pair<const char*, lua_CFunction> methods[] = {
        {"clear", gClear}, {"rect", gRect}, {"circle", gCircle}, {"line", gLine}, {"text", gText}, {"poly", gPoly}};
    for (auto [name, fn] : methods) {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, name);
    }
    lua_setfield(L, -2, "__index");
    lua_setfield(L, LUA_REGISTRYINDEX, "mf.canvas");

    lua_pushvalue(L, env);
    baseEnv_ = luaL_ref(L, LUA_REGISTRYINDEX);

    if (luaL_loadbuffer(L, kPrelude, std::strlen(kPrelude), "=prelude") == 0) {
        lua_pushvalue(L, env);
        lua_setfenv(L, -2);
        lua_call(L, 0, 0);
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 1);  // env

    // Until a script runs, expressions use a plain child of the base env.
    runScript("");
}

void LuaRuntime::setConstants(double fps, int width, int height, int sampleRate) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, baseEnv_);
    lua_pushnumber(L_, fps); lua_setfield(L_, -2, "fps");
    lua_pushnumber(L_, width); lua_setfield(L_, -2, "w");
    lua_pushnumber(L_, height); lua_setfield(L_, -2, "h");
    lua_pushnumber(L_, sampleRate); lua_setfield(L_, -2, "sample_rate");
    lua_getfield(L_, -1, "mf");
    lua_pushnumber(L_, fps); lua_setfield(L_, -2, "fps");
    lua_pushnumber(L_, width); lua_setfield(L_, -2, "width");
    lua_pushnumber(L_, height); lua_setfield(L_, -2, "height");
    lua_pushnumber(L_, sampleRate); lua_setfield(L_, -2, "sample_rate");
    lua_pop(L_, 2);
}

void LuaRuntime::setMarkers(const std::vector<Marker>& markers) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, baseEnv_);
    lua_createtable(L_, 0, int(markers.size()));
    for (const auto& m : markers) {
        lua_pushnumber(L_, m.time);
        lua_setfield(L_, -2, m.name.c_str());
    }
    lua_setfield(L_, -2, "marker");
    lua_pop(L_, 1);
}

void LuaRuntime::releaseCompiled() {
    for (auto& [key, ref] : compiled_)
        if (!key.starts_with("__fn:")) luaL_unref(L_, LUA_REGISTRYINDEX, ref);
    for (int ref : scriptRefs_) luaL_unref(L_, LUA_REGISTRYINDEX, ref);
    compiled_.clear();
    scriptRefs_.clear();
    signals_.clear();
    generators_.clear();
    synths_.clear();
}

int LuaRuntime::registerFunction(int stackIndex) {
    lua_pushvalue(L_, stackIndex);
    int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    scriptRefs_.push_back(ref);
    return ref;
}

bool LuaRuntime::pcall(int nargs, int nresults, std::string* err, double budgetSeconds) {
    g_deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                         std::chrono::duration<double>(budgetSeconds));
    lua_sethook(L_, budgetHook, LUA_MASKCOUNT, 10000);
    int rc = lua_pcall(L_, nargs, nresults, 0);
    lua_sethook(L_, nullptr, 0, 0);
    if (rc != 0) {
        if (err) {
            const char* msg = lua_tostring(L_, -1);
            *err = msg ? msg : "unknown Lua error";
        }
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

ScriptOutput LuaRuntime::runScript(const std::string& source) {
    releaseCompiled();
    if (scriptEnv_ != -2) luaL_unref(L_, LUA_REGISTRYINDEX, scriptEnv_);
    // Script globals live in their own table that falls back to the base env.
    lua_newtable(L_);
    lua_newtable(L_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, baseEnv_);
    lua_setfield(L_, -2, "__index");
    lua_setmetatable(L_, -2);
    scriptEnv_ = luaL_ref(L_, LUA_REGISTRYINDEX);

    ScriptOutput out;
    if (source.empty()) return out;
    pending_ = &out;
    if (luaL_loadbuffer(L_, source.data(), source.size(), "=script") != 0) {
        std::string msg = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        out.error = ScriptError{0, msg};
    } else {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, scriptEnv_);
        lua_setfenv(L_, -2);
        std::string err;
        // Interpreted, so the time budget hook can stop runaway loops.
        luaJIT_setmode(L_, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
        if (!pcall(0, 0, &err, 2.0)) out.error = ScriptError{0, err};
        luaJIT_setmode(L_, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_ON);
    }
    pending_ = nullptr;
    if (out.error) {
        // "script:12: message" -> line 12
        const std::string& m = out.error->message;
        if (m.starts_with("script:")) {
            size_t colon = m.find(':', 7);
            if (colon != std::string::npos) {
                out.error->line = std::atoi(m.substr(7, colon - 7).c_str());
                out.error->message = m.substr(colon + 1);
                if (!out.error->message.empty() && out.error->message[0] == ' ') out.error->message.erase(0, 1);
            }
        }
    }
    return out;
}

int LuaRuntime::compile(const std::string& expr, std::string* err) {
    if (auto it = compiled_.find(expr); it != compiled_.end()) return it->second;
    if (expr.starts_with("__fn:")) {
        if (err) *err = "script function is no longer defined";
        return -1;
    }
    std::string asExpr = "return " + expr;
    if (luaL_loadbuffer(L_, asExpr.data(), asExpr.size(), "=expr") != 0) {
        lua_pop(L_, 1);
        if (luaL_loadbuffer(L_, expr.data(), expr.size(), "=expr") != 0) {
            if (err) *err = lua_tostring(L_, -1);
            lua_pop(L_, 1);
            return -1;
        }
    }
    lua_rawgeti(L_, LUA_REGISTRYINDEX, scriptEnv_);
    lua_setfenv(L_, -2);
    int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    compiled_.emplace(expr, ref);
    return ref;
}

Value LuaRuntime::call(int ref, std::string* err) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    if (lua_type(L_, -1) != LUA_TFUNCTION) {
        Value v = luaToValue(L_, -1);
        lua_pop(L_, 1);
        return v;
    }
    if (!pcall(0, 1, err, 0.5)) return std::monostate{};
    Value v = luaToValue(L_, -1);
    lua_pop(L_, 1);
    return v;
}

Value LuaRuntime::callSignal(const std::string& name, std::string* err) {
    auto it = signals_.find(name);
    if (it == signals_.end()) {
        if (err) *err = "no signal " + name;
        return std::monostate{};
    }
    return call(it->second, err);
}

bool LuaRuntime::draw(const std::string& generator, DrawList& out, double t, double lt, double progress, int w, int h,
                      std::string* err) {
    auto it = generators_.find(generator);
    if (it == generators_.end()) {
        if (err) *err = "no generator '" + generator + "'";
        return false;
    }
    lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second);
    lua_createtable(L_, 0, 7);
    lua_pushlightuserdata(L_, &out); lua_setfield(L_, -2, "__dl");
    lua_pushnumber(L_, w); lua_setfield(L_, -2, "w");
    lua_pushnumber(L_, h); lua_setfield(L_, -2, "h");
    lua_pushnumber(L_, t); lua_setfield(L_, -2, "t");
    lua_pushnumber(L_, lt); lua_setfield(L_, -2, "lt");
    lua_pushnumber(L_, progress); lua_setfield(L_, -2, "progress");
    lua_getfield(L_, LUA_REGISTRYINDEX, "mf.canvas");
    lua_setmetatable(L_, -2);
    lua_pushvalue(L_, -1);
    int canvas = luaL_ref(L_, LUA_REGISTRYINDEX);
    bool ok = pcall(1, 0, err, 0.5);
    // Invalidate the canvas so a saved reference cannot write freed memory.
    lua_rawgeti(L_, LUA_REGISTRYINDEX, canvas);
    lua_pushnil(L_);
    lua_setfield(L_, -2, "__dl");
    lua_pop(L_, 1);
    luaL_unref(L_, LUA_REGISTRYINDEX, canvas);
    return ok;
}

bool LuaRuntime::synth(const std::string& name, double t0, double sampleRate, int frames, float* stereo,
                       std::string* err) {
    auto it = synths_.find(name);
    if (it == synths_.end()) {
        if (err) *err = "no synth '" + name + "'";
        return false;
    }
    // One protected call and deadline per block. The hook still interrupts a
    // runaway sample, and errors unwind to pcall without crossing C++ objects.
    lua_pushcclosure(L_, [](lua_State* L) -> int {
        const double t0 = lua_tonumber(L, 2), rate = lua_tonumber(L, 3);
        const int frames = int(lua_tointeger(L, 4));
        auto* stereo = static_cast<float*>(lua_touserdata(L, 5));
        for (int i = 0; i < frames; ++i) {
            lua_pushvalue(L, 1);
            lua_pushnumber(L, t0 + i / rate);
            lua_call(L, 1, 2);
            float l = float(lua_tonumber(L, -2));
            float r = lua_isnil(L, -1) ? l : float(lua_tonumber(L, -1));
            lua_pop(L, 2);
            stereo[i * 2] = std::isfinite(l) ? l : 0.0f;
            stereo[i * 2 + 1] = std::isfinite(r) ? r : 0.0f;
        }
        return 0;
    }, 0);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, it->second);
    lua_pushnumber(L_, t0);
    lua_pushnumber(L_, sampleRate);
    lua_pushinteger(L_, frames);
    lua_pushlightuserdata(L_, stereo);
    luaJIT_setmode(L_, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    bool ok = pcall(5, 0, err, 0.5);
    luaJIT_setmode(L_, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_ON);
    return ok;
}

Value LuaRuntime::eval(const std::string& code, std::string* err) {
    std::string asExpr = "return " + code;
    if (luaL_loadbuffer(L_, asExpr.data(), asExpr.size(), "=console") != 0) {
        lua_pop(L_, 1);
        if (luaL_loadbuffer(L_, code.data(), code.size(), "=console") != 0) {
            if (err) *err = lua_tostring(L_, -1);
            lua_pop(L_, 1);
            return std::monostate{};
        }
    }
    lua_rawgeti(L_, LUA_REGISTRYINDEX, scriptEnv_);
    lua_setfenv(L_, -2);
    luaJIT_setmode(L_, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    bool ok = pcall(0, 1, err, 2.0);
    luaJIT_setmode(L_, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_ON);
    if (!ok) return std::monostate{};
    Value v = luaToValue(L_, -1);
    lua_pop(L_, 1);
    return v;
}

namespace {

int declare(lua_State* L, std::map<std::string, int>& (*select)(LuaRuntime*), std::vector<std::string> ScriptOutput::*list,
            bool requireFunction) {
    LuaRuntime* rt = runtimeOf(L);
    ScriptOutput& out = pendingOr(L, rt);
    std::string name = luaL_checkstring(L, 1);
    if (lua_isnoneornil(L, 2)) return luaL_error(L, "%s needs a value", name.c_str());
    if (requireFunction) luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& map = select(rt);
    if (map.contains(name)) return luaL_error(L, "'%s' is defined twice", name.c_str());
    map[name] = rt->registerFunction(2);
    (out.*list).push_back(name);
    return 0;
}

}  // namespace

// Friend-free access to the private maps via small accessors.
struct LuaRuntimeAccess {
    static std::map<std::string, int>& signals(LuaRuntime* r) { return r->signals_; }
    static std::map<std::string, int>& generators(LuaRuntime* r) { return r->generators_; }
    static std::map<std::string, int>& synths(LuaRuntime* r) { return r->synths_; }
    static std::unordered_map<std::string, int>& compiled(LuaRuntime* r) { return r->compiled_; }
};

namespace {

int mfSignal(lua_State* L) { return declare(L, LuaRuntimeAccess::signals, &ScriptOutput::signals, false); }
int mfGenerator(lua_State* L) { return declare(L, LuaRuntimeAccess::generators, &ScriptOutput::generators, true); }
int mfSynth(lua_State* L) { return declare(L, LuaRuntimeAccess::synths, &ScriptOutput::synths, true); }

Property propFromLua(lua_State* L, LuaRuntime* rt, int index) {
    Property p;
    switch (lua_type(L, index)) {
        case LUA_TFUNCTION: {
            int ref = rt->registerFunction(index);
            std::string key = fmt::format("__fn:{}", ref);
            LuaRuntimeAccess::compiled(rt)[key] = ref;
            p.mode = PropMode::Expression;
            p.expr = key;
            return p;
        }
        case LUA_TTABLE: {
            lua_getfield(L, index, "expr");
            if (lua_isstring(L, -1)) {
                p.mode = PropMode::Expression;
                p.expr = lua_tostring(L, -1);
                lua_pop(L, 1);
                return p;
            }
            lua_pop(L, 1);
            lua_getfield(L, index, "curve");
            if (lua_istable(L, -1)) {
                p.mode = PropMode::Curve;
                int n = int(lua_objlen(L, -1));
                for (int i = 1; i <= n; ++i) {
                    lua_rawgeti(L, -1, i);
                    if (lua_istable(L, -1)) {
                        lua_rawgeti(L, -1, 1);
                        lua_rawgeti(L, -2, 2);
                        lua_rawgeti(L, -3, 3);
                        Keyframe k;
                        k.t = lua_tonumber(L, -3);
                        k.v = lua_tonumber(L, -2);
                        if (lua_isstring(L, -1)) {
                            std::string mode = lua_tostring(L, -1);
                            if (auto in = parseInterp(mode)) k.interp = *in;
                            else if (const EasePreset* e = findEase(mode)) {
                                k.interp = Interp::Bezier;
                                k.x1 = e->x1; k.y1 = e->y1; k.x2 = e->x2; k.y2 = e->y2;
                            }
                        }
                        lua_pop(L, 3);
                        p.curve.keys.push_back(k);
                    }
                    lua_pop(L, 1);
                }
                p.curve.sort();
                lua_pop(L, 1);
                return p;
            }
            lua_pop(L, 1);
            p.constant = luaToValue(L, index);
            return p;
        }
        default: p.constant = luaToValue(L, index); return p;
    }
}

int mfClip(lua_State* L) {
    LuaRuntime* rt = runtimeOf(L);
    ScriptOutput& out = pendingOr(L, rt);
    luaL_checktype(L, 1, LUA_TTABLE);
    ScriptClipSpec spec;
    bool kindSet = false;
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        // key at -2, value at -1
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 1);
            continue;
        }
        std::string key = lua_tostring(L, -2);
        if (key == "track") {
            if (lua_type(L, -1) == LUA_TNUMBER) spec.trackIndex = int(lua_tonumber(L, -1)) - 1;
            else spec.track = luaL_checkstring(L, -1);
        } else if (key == "kind") {
            auto k = parseClipKind(luaL_checkstring(L, -1));
            if (!k) return luaL_error(L, "unknown clip kind '%s'", lua_tostring(L, -1));
            spec.kind = *k;
            kindSet = true;
        } else if (key == "name") {
            spec.name = luaL_checkstring(L, -1);
        } else if (key == "media") {
            spec.media = luaL_checkstring(L, -1);
        } else if (key == "start") {
            spec.start = luaL_checknumber(L, -1);
        } else if (key == "duration") {
            spec.duration = luaL_checknumber(L, -1);
        } else if (key == "in") {
            spec.in = luaL_checknumber(L, -1);
        } else if (key == "speed") {
            spec.speed = luaL_checknumber(L, -1);
        } else if (key == "props") {
            luaL_checktype(L, -1, LUA_TTABLE);
            int props = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, props) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING) spec.props[lua_tostring(L, -2)] = propFromLua(L, rt, lua_gettop(L));
                lua_pop(L, 1);
            }
        } else {
            spec.props[key] = propFromLua(L, rt, lua_gettop(L));
        }
        lua_pop(L, 1);
    }
    if (!kindSet) {
        if (!spec.media.empty()) spec.kind = ClipKind::Media;
        else if (spec.props.contains("generator")) spec.kind = ClipKind::Generator;
        else if (spec.props.contains("synth")) spec.kind = ClipKind::Synth;
        else if (spec.props.contains("text")) spec.kind = ClipKind::Text;
        else if (spec.props.contains("path") || spec.props.contains("shape")) spec.kind = ClipKind::Path;
        else spec.kind = ClipKind::Solid;
    }
    out.clips.push_back(std::move(spec));
    lua_pushinteger(L, lua_Integer(out.clips.size()));
    return 1;
}

}  // namespace

}  // namespace mf
