#include "render/path.hpp"

#include "core/work_counter.hpp"

#include <libxml/parser.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <sstream>
#include <stdexcept>
namespace mf {
namespace {
constexpr double pi = std::numbers::pi;
constexpr size_t maxPoints = 32768;
[[noreturn]] void fail(const std::string& s) { throw std::runtime_error("path: " + s); }
PathPoint mix(PathPoint a, PathPoint b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
double dist(PathPoint a, PathPoint b) { return std::hypot(a.x - b.x, a.y - b.y); }
class Reader {
   public:
    std::string s;
    size_t i = 0;
    explicit Reader(std::string_view v) : s(v) {
        if (s.size() > 1024 * 1024) fail("input exceeds 1 MiB");
    }
    void space() {
        while (i < s.size() && (std::isspace((unsigned char)s[i]) || s[i] == ',')) ++i;
    }
    bool end() {
        space();
        return i == s.size();
    }
    bool number() {
        space();
        return i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.');
    }
    double num() {
        space();
        if (!number()) fail("expected number");
        char* e = nullptr;
        double n = std::strtod(s.c_str() + i, &e);
        if (e == s.c_str() + i || !std::isfinite(n) || std::abs(n) > 1e7) fail("invalid or excessive coordinate");
        i = size_t(e - s.c_str());
        return n;
    }
};
size_t edges(const PathContour& c) { return c.points.size() < 2 ? 0 : c.points.size() - 1 + (c.closed ? 1 : 0); }
PathPoint next(const PathContour& c, size_t i) { return c.points[(i + 1) % c.points.size()]; }
double length(const PathContour& c) {
    double n = 0;
    for (size_t i = 0; i < edges(c); ++i) n += dist(c.points[i], next(c, i));
    return n;
}
PathPoint sample(const PathContour& c, double t) {
    if (c.points.empty()) fail("empty contour");
    double left = std::clamp(t, 0., 1.) * length(c);
    for (size_t i = 0; i < edges(c); ++i) {
        double l = dist(c.points[i], next(c, i));
        if (left <= l && l > 0) return mix(c.points[i], next(c, i), left / l);
        left -= l;
    }
    return c.closed ? c.points.front() : c.points.back();
}
}  // namespace
VectorPath parsePath(std::string_view d) {
    Reader r(d);
    VectorPath p;
    PathPoint cur{}, start{}, control{};
    char cmd = 0, previous = 0;
    size_t total = 0;
    auto add = [&](PathPoint q) {
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || std::abs(q.x) > 1e8 || std::abs(q.y) > 1e8)
            fail("excessive geometry");
        if (++total > maxPoints) fail("too many points");
        if (p.contours.empty()) fail("path must start with M");
        if (!p.contours.back().points.empty() && dist(p.contours.back().points.back(), q) == 0) {
            cur = q;
            return;
        }
        p.contours.back().points.push_back(q);
        cur = q;
    };
    while (!r.end()) {
        if (std::isalpha((unsigned char)r.s[r.i]))
            cmd = r.s[r.i++];
        else if (!cmd)
            fail("expected command");
        bool rel = std::islower((unsigned char)cmd);
        char op = char(std::toupper((unsigned char)cmd));
        PathPoint base = cur;
        auto xy = [&]() {
            double x = r.num(), y = r.num();
            return PathPoint{x + (rel ? base.x : 0), y + (rel ? base.y : 0)};
        };
        if (op == 'Z') {
            if (p.contours.empty()) fail("close without move");
            if (p.contours.back().points.size() > 1 &&
                dist(p.contours.back().points.front(), p.contours.back().points.back()) == 0)
                p.contours.back().points.pop_back();
            p.contours.back().closed = true;
            cur = start;
            previous = 'Z';
            cmd = 0;
            continue;
        }
        if (op == 'M') {
            auto q = xy();
            p.contours.push_back({});
            add(q);
            start = q;
            cmd = rel ? 'l' : 'L';
            previous = 'M';
            continue;
        }
        if (p.contours.empty()) fail("path must start with M");
        if (previous == 'Z') {
            p.contours.push_back({});
            add(base);
            start = base;
        }
        if (op == 'L')
            add(xy());
        else if (op == 'H') {
            double x = r.num();
            add({x + (rel ? base.x : 0), base.y});
        } else if (op == 'V') {
            double y = r.num();
            add({base.x, y + (rel ? base.y : 0)});
        } else if (op == 'C' || op == 'S' || op == 'Q' || op == 'T') {
            bool cubic = op == 'C' || op == 'S';
            PathPoint c1{}, c2{}, end{};
            if (op == 'C' || op == 'Q')
                c1 = xy();
            else
                c1 = ((cubic && (previous == 'C' || previous == 'S')) ||
                      (!cubic && (previous == 'Q' || previous == 'T')))
                         ? PathPoint{2 * base.x - control.x, 2 * base.y - control.y}
                         : base;
            if (cubic) c2 = xy();
            end = xy();
            double l = dist(base, c1) + dist(c1, cubic ? c2 : end) + (cubic ? dist(c2, end) : 0);
            int n = std::clamp(int(std::ceil(std::sqrt(l) * 3)), 8, 512);
            for (int j = 1; j <= n; ++j) {
                double t = double(j) / n, u = 1 - t;
                if (cubic)
                    add({u * u * u * base.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * end.x,
                         u * u * u * base.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * end.y});
                else
                    add({u * u * base.x + 2 * u * t * c1.x + t * t * end.x,
                         u * u * base.y + 2 * u * t * c1.y + t * t * end.y});
            }
            control = cubic ? c2 : c1;
        } else if (op == 'A') {
            auto flag = [&]() {
                r.space();
                if (r.i == r.s.size() || (r.s[r.i] != '0' && r.s[r.i] != '1')) fail("arc flag must be 0 or 1");
                return double(r.s[r.i++] - '0');
            };
            double rx = std::abs(r.num()), ry = std::abs(r.num()), phi = r.num() * pi / 180, large = flag(),
                   sweep = flag();
            if ((large != 0 && large != 1) || (sweep != 0 && sweep != 1)) fail("arc flags must be 0 or 1");
            auto end = xy();
            if (rx == 0 || ry == 0)
                add(end);
            else if (dist(base, end) > 1e-12) {
                double co = std::cos(phi), si = std::sin(phi), dx = (base.x - end.x) / 2, dy = (base.y - end.y) / 2,
                       x = co * dx + si * dy, y = -si * dx + co * dy;
                double norm = std::hypot(x / rx, y / ry);
                if (!std::isfinite(norm)) fail("arc radii ratio exceeds numeric range");
                if (norm > 1) {
                    rx *= norm;
                    ry *= norm;
                }
                double nx = x / rx, ny = y / ry, norm2 = nx * nx + ny * ny;
                if (!std::isfinite(norm2) || norm2 <= 0) fail("unstable arc geometry");
                double f = std::sqrt(std::max(0., 1 / norm2 - 1)) * (large == sweep ? -1 : 1), cx = f * rx * ny,
                       cy = -f * ry * nx;
                double centerX = co * cx - si * cy + (base.x + end.x) / 2,
                       centerY = si * cx + co * cy + (base.y + end.y) / 2;
                if (!std::isfinite(centerX) || !std::isfinite(centerY) || !std::isfinite(rx) || !std::isfinite(ry) ||
                    rx > 1e8 || ry > 1e8)
                    fail("excessive arc geometry");
                double a = std::atan2((y - cy) / ry, (x - cx) / rx), b = std::atan2((-y - cy) / ry, (-x - cx) / rx),
                       delta = b - a;
                if (sweep && delta < 0) delta += 2 * pi;
                if (!sweep && delta > 0) delta -= 2 * pi;
                int n = std::clamp(int(std::ceil(std::abs(delta) * std::sqrt(std::max(rx, ry)) * 2)), 8, 1024);
                for (int j = 1; j <= n; ++j) {
                    double q = a + delta * j / n;
                    add({centerX + co * rx * std::cos(q) - si * ry * std::sin(q),
                         centerY + si * rx * std::cos(q) + co * ry * std::sin(q)});
                }
            }
        } else
            fail(std::string("unsupported command ") + cmd);
        previous = op;
    }
    if (p.contours.empty()) fail("empty path");
    return p;
}
VectorPath shapePath(std::string_view shape, double w, double h) {
    if (!std::isfinite(w) || !std::isfinite(h) || w < 0 || h < 0 || w > 1e7 || h > 1e7) fail("invalid shape size");
    VectorPath p;
    PathContour c;
    if (shape == "rectangle")
        c = {{{-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2, h / 2}, {-w / 2, h / 2}}, true};
    else if (shape == "triangle")
        c = {{{0, -h / 2}, {w / 2, h / 2}, {-w / 2, h / 2}}, true};
    else if (shape == "line")
        c = {{{-w / 2, -h / 2}, {w / 2, h / 2}}, false};
    else if (shape == "ellipse" || shape == "circle") {
        c.closed = true;
        for (int i = 0; i < 128; ++i) {
            double a = 2 * pi * i / 128;
            c.points.push_back({w / 2 * std::cos(a), h / 2 * std::sin(a)});
        }
    } else
        fail("unknown shape");
    p.contours.push_back(c);
    return p;
}
VectorPath morphPath(const VectorPath& a, const VectorPath& b, double t) {
    if (a.contours.size() != b.contours.size()) fail("morph requires matching contour counts");
    VectorPath p;
    if (!std::isfinite(t)) fail("morph progress must be finite");
    size_t total = 0;
    t = std::clamp(t, 0., 1.);
    for (size_t i = 0; i < a.contours.size(); ++i) {
        auto& x = a.contours[i];
        auto& y = b.contours[i];
        if (x.closed != y.closed || x.points.empty() || y.points.empty())
            fail("morph requires corresponding open/closed contours");
        PathContour c;
        c.closed = x.closed;
        size_t n = std::clamp(std::max(x.points.size(), y.points.size()), size_t(64), size_t(2048));
        if ((total += n) > maxPoints) fail("morph exceeds point limit");
        for (size_t j = 0; j < n; ++j) {
            double q = double(j) / (c.closed ? n : n - 1);
            c.points.push_back(mix(sample(x, q), sample(y, q), t));
        }
        p.contours.push_back(std::move(c));
    }
    return p;
}
PathPoint pointOnPath(const VectorPath& p, double progress) {
    if (!std::isfinite(progress)) fail("path progress must be finite");
    double l = 0;
    for (auto& c : p.contours) l += length(c);
    double left = std::clamp(progress, 0., 1.) * l;
    for (auto& c : p.contours) {
        double n = length(c);
        if (left <= n && !c.points.empty()) return sample(c, n ? left / n : 0);
        left -= n;
    }
    fail("empty path");
}
double pathAngle(const VectorPath& p, double t) {
    auto a = pointOnPath(p, std::max(0., t - 0.0001)), b = pointOnPath(p, std::min(1., t + 0.0001));
    return std::atan2(b.y - a.y, b.x - a.x) * 180 / pi;
}
std::string pathData(const VectorPath& p) {
    std::ostringstream s;
    s << std::setprecision(12);
    for (auto& c : p.contours) {
        for (size_t i = 0; i < c.points.size(); ++i) s << (i ? " L" : "M") << c.points[i].x << ' ' << c.points[i].y;
        if (c.closed) s << " Z";
        s << ' ';
    }
    return s.str();
}
namespace {
void blend(uint8_t* o, Color c, double coverage) {
    double a = std::clamp(double(c.a) * coverage, 0., 1.), inv = 1 - a;
    o[0] = uint8_t(std::clamp(c.r * a * 255 + o[0] * inv, 0., 255.));
    o[1] = uint8_t(std::clamp(c.g * a * 255 + o[1] * inv, 0., 255.));
    o[2] = uint8_t(std::clamp(c.b * a * 255 + o[2] * inv, 0., 255.));
    o[3] = uint8_t(std::clamp(a * 255 + o[3] * inv, 0., 255.));
}
}  // namespace
void drawPath(Image& dst, const VectorPath& path, double unit, Color fill, Color stroke, double width, double trimStart,
              double trimEnd, std::string_view rule, std::string_view linecap, std::string_view linejoin,
              double miterLimit) {
    if (dst.empty() || !std::isfinite(unit) || unit <= 0) return;
    if (rule != "nonzero" && rule != "evenodd") fail("fill rule must be nonzero or evenodd");
    VectorPath p = path;
    size_t count = 0;
    for (auto& c : p.contours)
        for (auto& q : c.points) {
            if (++count > maxPoints) fail("too many render points");
            q = {q.x * unit + dst.w / 2., q.y * unit + dst.h / 2.};
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || std::abs(q.x) > 1e9 || std::abs(q.y) > 1e9)
                fail("excessive render geometry");
        }
    if (!std::isfinite(trimStart) || !std::isfinite(trimEnd)) fail("trim values must be finite");
    trimStart = std::clamp(trimStart, 0., 1.);
    trimEnd = std::clamp(trimEnd, 0., 1.);
    // During a stroke reveal, defer the fill until the full contour is visible.
    if (fill.a > 0 && trimStart == 0 && trimEnd == 1) {
        std::vector<double> coverage(dst.w);
        std::vector<std::pair<double, int>> hits;
        struct FillEdge {
            PathPoint a, b;
            double lower, upper;
        };
        std::vector<FillEdge> edges;
        size_t originalEdgeCount = 0;
        for (const auto& contour : p.contours) {
            if (contour.points.size() < 3) continue;
            originalEdgeCount += contour.points.size();
            for (size_t i = 0; i < contour.points.size(); ++i) {
                auto a = contour.points[i], b = contour.points[(i + 1) % contour.points.size()];
                if (a.y != b.y) edges.push_back({a, b, std::min(a.y, b.y), std::max(a.y, b.y)});
            }
        }
        // Keep the existing geometry limit even though inactive edges no longer
        // need a test on every subrow.
        if (uint64_t(originalEdgeCount) * 4 * dst.h > 100000000) fail("fill exceeds render work limit");
        std::sort(edges.begin(), edges.end(), [](const FillEdge& a, const FillEdge& b) { return a.lower < b.lower; });
        std::vector<const FillEdge*> active;
        active.reserve(edges.size());
        size_t nextEdge = 0;
        uint64_t edgeSteps = edges.size();
        for (int y = 0; y < dst.h; ++y) {
            std::fill(coverage.begin(), coverage.end(), 0.);
            for (int sub = 0; sub < 4; ++sub) {
                double yy = y + (sub + .5) / 4;
                hits.clear();
                std::erase_if(active, [&](const FillEdge* edge) { return edge->upper <= yy; });
                while (nextEdge < edges.size() && edges[nextEdge].lower <= yy) {
                    const auto& edge = edges[nextEdge++];
                    if (edge.upper > yy) active.push_back(&edge);
                }
                edgeSteps += active.size();
                for (const FillEdge* edge : active) {
                    const auto& a = edge->a;
                    const auto& b = edge->b;
                    hits.push_back({a.x + (yy - a.y) * (b.x - a.x) / (b.y - a.y), b.y > a.y ? 1 : -1});
                }
                std::sort(hits.begin(), hits.end());
                int winding = 0;
                double left = 0;
                for (auto [x, dir] : hits) {
                    bool before = rule == "evenodd" ? (winding % 2 != 0) : (winding != 0);
                    winding += dir;
                    bool after = rule == "evenodd" ? (winding % 2 != 0) : (winding != 0);
                    if (!before && after) left = x;
                    if (before && !after) {
                        double l = std::clamp(left, 0., double(dst.w)), r = std::clamp(x, 0., double(dst.w));
                        int spanStart = int(std::floor(l)), spanEnd = std::min(dst.w, int(std::ceil(r)));
                        work::addRaster(uint64_t(std::max(0, spanEnd - spanStart)));
                        for (int xx = spanStart; xx < spanEnd; ++xx)
                            coverage[xx] += std::max(0., std::min(r, double(xx + 1)) - std::max(l, double(xx))) / 4;
                    }
                }
            }
            work::addMap(uint64_t(dst.w));
            for (int x = 0; x < dst.w; ++x)
                if (coverage[x] > 0) blend(dst.at(x, y), fill, coverage[x]);
        }
        work::addEdge(edgeSteps);
    }
    if (stroke.a <= 0 || width <= 0 || trimEnd <= trimStart) return;
    if (linecap != "round" && linecap != "butt" && linecap != "square") fail("invalid stroke cap");
    if (linejoin != "round" && linejoin != "bevel" && linejoin != "miter") fail("invalid stroke join");
    if (!std::isfinite(miterLimit) || miterLimit < 1 || miterLimit > 100) fail("miter limit must be 1..100");
    double radius = width * unit / 2;
    if (!std::isfinite(radius) || radius > 1e7) fail("excessive stroke width");
    std::vector<float> mask(size_t(dst.w) * dst.h, 0);
    std::vector<uint8_t> samples(dst.w);
    std::vector<double> crossings;
    crossings.reserve(4);
    size_t work = 0;
    auto polygon = [&](const std::vector<PathPoint>& pts) {
        double lx = dst.w, ly = dst.h, hx = 0, hy = 0;
        for (auto q : pts) {
            lx = std::min(lx, q.x);
            ly = std::min(ly, q.y);
            hx = std::max(hx, q.x);
            hy = std::max(hy, q.y);
        }
        int x0 = int(std::clamp(std::floor(lx - 1), 0., double(dst.w))),
            x1 = int(std::clamp(std::ceil(hx + 1), 0., double(dst.w)));
        int y0 = int(std::clamp(std::floor(ly - 1), 0., double(dst.h))),
            y1 = int(std::clamp(std::ceil(hy + 1), 0., double(dst.h)));
        if (x0 >= x1 || y0 >= y1) return;
        size_t pixels = size_t(x1 - x0) * (y1 - y0);
        if (pixels > 100000000 - work) fail("stroke exceeds render work limit");
        work += pixels;
        work::addEdge(uint64_t(pts.size()) * 4 * (y1 - y0));
        uint64_t rasterized = 0;
        for (int y = y0; y < y1; ++y) {
            std::fill(samples.begin() + x0, samples.begin() + x1, uint8_t(0));
            int maskBegin = x1, maskEnd = x0;
            for (int sy = 0; sy < 4; ++sy) {
                double yy = y + (sy + .5) / 4;
                crossings.clear();
                for (size_t j = 0, k = pts.size() - 1; j < pts.size(); k = j++) {
                    auto a = pts[j], b = pts[k];
                    if ((a.y > yy) != (b.y > yy))
                        crossings.push_back((b.x - a.x) * (yy - a.y) / (b.y - a.y) + a.x);
                }
                std::sort(crossings.begin(), crossings.end());
                for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
                    // Keep the original 4x4 grid exactly: sample s sits at
                    // (s + .5) / 4, and the filled interval is [left, right).
                    int begin = int(std::clamp(std::ceil(crossings[i] * 4 - .5), double(x0 * 4), double(x1 * 4)));
                    int end = int(std::clamp(std::ceil(crossings[i + 1] * 4 - .5), double(x0 * 4), double(x1 * 4)));
                    if (begin >= end) continue;
                    int first = begin / 4, last = (end + 3) / 4;
                    maskBegin = std::min(maskBegin, first);
                    maskEnd = std::max(maskEnd, last);
                    for (int x = first; x < last; ++x)
                        samples[x] += uint8_t(std::min(end, (x + 1) * 4) - std::max(begin, x * 4));
                }
            }
            if (maskBegin >= maskEnd) continue;
            rasterized += maskEnd - maskBegin;
            for (int x = maskBegin; x < maskEnd; ++x) {
                auto& v = mask[size_t(y) * dst.w + x];
                v = std::max(v, samples[x] / 16.f);
            }
        }
        work::addRaster(rasterized);
    };
    auto disc = [&](PathPoint a) {
        int x0 = int(std::clamp(std::floor(a.x - radius - 1), 0., double(dst.w))),
            x1 = int(std::clamp(std::ceil(a.x + radius + 1), 0., double(dst.w)));
        int y0 = int(std::clamp(std::floor(a.y - radius - 1), 0., double(dst.h))),
            y1 = int(std::clamp(std::ceil(a.y + radius + 1), 0., double(dst.h)));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                if (++work > 100000000) fail("stroke exceeds render work limit");
                work::addTaps(1);
                auto& v = mask[size_t(y) * dst.w + x];
                v = std::max(v, float(std::clamp(radius + .5 - std::hypot(x + .5 - a.x, y + .5 - a.y), 0., 1.)));
            }
    };
    double total = 0;
    for (auto& c : p.contours) total += length(c);
    double cursor = 0;
    for (auto& c : p.contours) {
        bool fullClosed = c.closed && trimStart * total <= cursor && trimEnd * total >= cursor + length(c);
        for (size_t i = 0; i < edges(c); ++i) {
            auto a = c.points[i], b = next(c, i);
            double l = dist(a, b), from = std::max(cursor, total * trimStart),
                   to = std::min(cursor + l, total * trimEnd);
            if (l > 0 && to > from) {
                auto aa = mix(a, b, (from - cursor) / l), bb = mix(a, b, (to - cursor) / l);
                double ux = (b.x - a.x) / l, uy = (b.y - a.y) / l, nx = -uy * radius, ny = ux * radius;
                bool first = !fullClosed && (from == total * trimStart || i == 0),
                     last = !fullClosed && (to == total * trimEnd || i + 1 == edges(c));
                if (linecap == "square") {
                    if (first) {
                        aa.x -= ux * radius;
                        aa.y -= uy * radius;
                    }
                    if (last) {
                        bb.x += ux * radius;
                        bb.y += uy * radius;
                    }
                }
                polygon(
                    {{aa.x + nx, aa.y + ny}, {bb.x + nx, bb.y + ny}, {bb.x - nx, bb.y - ny}, {aa.x - nx, aa.y - ny}});
                if (linecap == "round") {
                    if (first) disc(aa);
                    if (last) disc(bb);
                }
                // Join the outgoing segment only when that vertex is inside the reveal.
                bool join = i + 1 < edges(c) && to == cursor + l && to < total * trimEnd;
                if (fullClosed) join = true;
                if (join) {
                    auto cc = c.points[(i + 2) % c.points.size()];
                    double l2 = dist(b, cc);
                    if (l2 > 0) {
                        double vx = (cc.x - b.x) / l2, vy = (cc.y - b.y) / l2, cross = ux * vy - uy * vx;
                        if (linejoin == "round")
                            disc(b);
                        else if (std::abs(cross) > 1e-10) {
                            double side = cross > 0 ? -1 : 1;
                            PathPoint n1{-uy * radius * side, ux * radius * side},
                                n2{-vy * radius * side, vx * radius * side}, a1{b.x + n1.x, b.y + n1.y},
                                a2{b.x + n2.x, b.y + n2.y};
                            std::vector<PathPoint> pts{b, a1};
                            if (linejoin == "miter") {
                                double t = ((a2.x - a1.x) * vy - (a2.y - a1.y) * vx) / cross;
                                PathPoint tip{a1.x + t * ux, a1.y + t * uy};
                                if (dist(tip, b) <= radius * miterLimit) pts.push_back(tip);
                            }
                            pts.push_back(a2);
                            polygon(pts);
                        }
                    }
                }
            }
            cursor += l;
        }
    }
    work::addMap(mask.size());
    for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i] > 0) blend(dst.px.data() + i * 4, stroke, mask[i]);
}
namespace {
using Matrix = std::array<double, 6>;
Matrix mul(Matrix a, Matrix b) {
    return {a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],        a[0] * b[2] + a[2] * b[3],
            a[1] * b[2] + a[3] * b[3], a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5]};
}
Matrix transform(std::string_view text) {
    Reader r(text);
    Matrix out{1, 0, 0, 1, 0, 0};
    while (!r.end()) {
        size_t begin = r.i;
        while (r.i < r.s.size() && std::isalpha((unsigned char)r.s[r.i])) ++r.i;
        auto name = r.s.substr(begin, r.i - begin);
        r.space();
        if (r.i == r.s.size() || r.s[r.i++] != '(') fail("invalid SVG transform");
        std::vector<double> v;
        while (true) {
            r.space();
            if (r.i < r.s.size() && r.s[r.i] == ')') {
                ++r.i;
                break;
            }
            if (v.size() == 6) fail("invalid SVG transform arguments");
            v.push_back(r.num());
        }
        Matrix m{1, 0, 0, 1, 0, 0};
        if (name == "matrix" && v.size() == 6)
            std::copy(v.begin(), v.end(), m.begin());
        else if (name == "translate" && (v.size() == 1 || v.size() == 2)) {
            m[4] = v[0];
            m[5] = v.size() == 2 ? v[1] : 0;
        } else if (name == "scale" && (v.size() == 1 || v.size() == 2)) {
            m[0] = v[0];
            m[3] = v.size() == 2 ? v[1] : v[0];
        } else if (name == "rotate" && (v.size() == 1 || v.size() == 3)) {
            double a = v[0] * pi / 180;
            m = {std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0};
            if (v.size() == 3) m = mul(mul({1, 0, 0, 1, v[1], v[2]}, m), {1, 0, 0, 1, -v[1], -v[2]});
        } else if ((name == "skewX" || name == "skewY") && v.size() == 1)
            m[name == "skewX" ? 2 : 1] = std::tan(v[0] * pi / 180);
        else
            fail("unsupported SVG transform");
        out = mul(out, m);
    }
    return out;
}
using Attrs = std::map<std::string, std::string>;
std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
Attrs attrs(xmlNode* n) {
    Attrs a;
    for (xmlAttr* p = n->properties; p; p = p->next) {
        xmlChar* value = xmlNodeListGetString(n->doc, p->children, 1);
        if (value) {
            a[(const char*)p->name] = (const char*)value;
            xmlFree(value);
        }
    }
    if (a.contains("style")) {
        std::istringstream s(a["style"]);
        std::string part;
        while (std::getline(s, part, ';')) {
            auto k = part.find(':');
            if (k == std::string::npos) {
                if (!trim(part).empty()) fail("invalid SVG style");
                continue;
            }
            a[trim(part.substr(0, k))] = trim(part.substr(k + 1));
        }
        a.erase("style");
    }
    return a;
}
double scalar(const std::string& s) {
    Reader r(s);
    double v = r.num();
    r.space();
    if (r.s.substr(r.i) == "px") r.i += 2;
    if (!r.end()) fail("SVG lengths require plain numbers or px");
    return v;
}
Color paint(const std::string& s) {
    if (s == "none") return {0, 0, 0, 0};
    if (s == "black") return {0, 0, 0, 1};
    if (s == "white") return {1, 1, 1, 1};
    if (s == "red") return {1, 0, 0, 1};
    if (s == "green") return {0, .50196f, 0, 1};
    if (s == "blue") return {0, 0, 1, 1};
    std::string t = s;
    if (t.size() == 4 && t[0] == '#') t = std::string("#") + t[1] + t[1] + t[2] + t[2] + t[3] + t[3];
    if (auto c = parseColor(t)) return *c;
    fail("unsupported SVG paint: " + s);
}
}  // namespace
std::vector<ImportedPath> importPaths(const std::string& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in) fail("cannot open " + filename);
    std::string source;
    source.resize(1024 * 1024 + 1);
    in.read(source.data(), source.size());
    source.resize(size_t(in.gcount()));
    if (source.size() > 1024 * 1024) fail("file exceeds 1 MiB");
    auto dot = filename.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : filename.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".path" || ext == ".txt") return {{"path", pathData(parsePath(source))}};
    if (ext != ".svg") fail("supported imports: .svg, .path, .txt");
    xmlDoc* raw = xmlReadMemory(source.data(), int(source.size()), filename.c_str(), nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!raw) fail("invalid SVG XML");
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(raw, xmlFreeDoc);
    if (raw->intSubset || raw->extSubset) fail("SVG document types are unsupported");
    xmlNode* root = xmlDocGetRootElement(raw);
    if (!root || std::string((char*)root->name) != "svg") fail("expected SVG root");
    Attrs rootAttrs = attrs(root);
    double cx = 0, cy = 0;
    Matrix viewport{1, 0, 0, 1, 0, 0};
    if (rootAttrs.contains("viewBox")) {
        Reader r(rootAttrs["viewBox"]);
        double x = r.num(), y = r.num(), w = r.num(), h = r.num();
        if (!r.end() || w <= 0 || h <= 0) fail("invalid SVG viewBox");
        double vw = rootAttrs.contains("width") ? scalar(rootAttrs["width"]) : w;
        double vh = rootAttrs.contains("height") ? scalar(rootAttrs["height"]) : h;
        if (vw <= 0 || vh <= 0) fail("SVG viewport dimensions must be positive");
        cx = vw / 2;
        cy = vh / 2;
        double sx = vw / w, sy = vh / h, tx = 0, ty = 0;
        std::string aspect =
            rootAttrs.contains("preserveAspectRatio") ? trim(rootAttrs["preserveAspectRatio"]) : "xMidYMid meet";
        if (aspect != "none") {
            std::istringstream spec(aspect);
            std::string align, mode = "meet", extra;
            spec >> align;
            if (spec >> mode) {
                if (spec >> extra) fail("invalid preserveAspectRatio");
            }
            if ((mode != "meet" && mode != "slice") || align.size() != 8 || align[0] != 'x' || align[4] != 'Y')
                fail("unsupported preserveAspectRatio");
            auto offset = [](const std::string& alignment) {
                if (alignment == "Min") return 0.;
                if (alignment == "Mid") return .5;
                if (alignment == "Max") return 1.;
                fail("unsupported SVG viewport alignment");
            };
            double ax = offset(align.substr(1, 3)), ay = offset(align.substr(5, 3));
            sx = sy = mode == "meet" ? std::min(sx, sy) : std::max(sx, sy);
            tx = (vw - w * sx) * ax;
            ty = (vh - h * sy) * ay;
        }
        viewport = {sx, 0, 0, sy, tx - x * sx, ty - y * sy};
    } else {
        if (rootAttrs.contains("width")) cx = scalar(rootAttrs["width"]) / 2;
        if (rootAttrs.contains("height")) cy = scalar(rootAttrs["height"]) / 2;
        if (cx < 0 || cy < 0) fail("SVG viewport dimensions must be positive");
        if (rootAttrs.contains("preserveAspectRatio")) fail("preserveAspectRatio requires viewBox");
    }
    std::vector<ImportedPath> result;
    size_t nodes = 0, points = 0;
    std::function<void(xmlNode*, Matrix, Attrs, int)> walk = [&](xmlNode* n, Matrix parent, Attrs inherited,
                                                                 int depth) {
        if (depth > 64) fail("SVG nesting exceeds 64");
        for (; n; n = n->next) {
            if (n->type == XML_COMMENT_NODE || n->type == XML_TEXT_NODE) continue;
            if (n->type != XML_ELEMENT_NODE) fail("unsupported XML node");
            if (++nodes > 4096) fail("SVG has too many elements");
            std::string tag = (char*)n->name;
            Attrs a = attrs(n);
            Matrix m = parent;
            if (a.contains("transform")) m = mul(m, transform(a["transform"]));
            for (auto& [k, v] : a) {
                if (k == "fill" || k == "stroke" || k == "stroke-width" || k == "fill-rule" || k == "fill-opacity" ||
                    k == "stroke-opacity" || k == "stroke-linecap" || k == "stroke-linejoin" ||
                    k == "stroke-miterlimit")
                    inherited[k] = v;
                else if (k == "opacity" && (tag == "g" || tag == "svg") && scalar(v) != 1)
                    fail("SVG group opacity is unsupported");
                else if (k == "display" && v != "inline")
                    fail("SVG display is unsupported");
                else if (k == "visibility" && v != "visible")
                    fail("SVG visibility is unsupported");
                else if (k == "filter" || k == "clip-path" || k == "mask" || k == "stroke-dasharray" ||
                         k == "stroke-dashoffset" || k == "vector-effect" || k == "marker-start" || k == "marker-mid" ||
                         k == "marker-end" || k == "class")
                    fail("unsupported SVG attribute: " + k);
                else if (k != "id" && k != "transform" && k != "opacity" && k != "stroke-linecap" &&
                         k != "stroke-linejoin" && k != "display" && k != "visibility" && k != "d" && k != "x" &&
                         k != "y" && k != "x1" && k != "x2" && k != "y1" && k != "y2" && k != "width" &&
                         k != "height" && k != "cx" && k != "cy" && k != "r" && k != "rx" && k != "ry" &&
                         k != "points" && k != "viewBox" && k != "version" && k != "preserveAspectRatio" &&
                         k != "xmlns")
                    fail("unsupported SVG attribute: " + k);
            }
            if (tag == "svg" || tag == "g") {
                if (tag == "svg" && n != root) fail("nested SVG is unsupported");
                walk(n->children, m, inherited, depth + 1);
                continue;
            }
            if (tag == "title" || tag == "desc" || tag == "metadata") continue;
            auto number = [&](const char* k, double fallback = 0.) { return a.contains(k) ? scalar(a[k]) : fallback; };
            VectorPath p;
            if (tag == "path") {
                if (!a.contains("d")) fail("SVG path missing d");
                p = parsePath(a["d"]);
            } else if (tag == "rect") {
                double w = number("width"), h = number("height");
                double rx = number("rx", number("ry")), ry = number("ry", rx);
                if (rx < 0 || ry < 0) fail("negative rectangle radius");
                rx = std::min(rx, w / 2);
                ry = std::min(ry, h / 2);
                if (rx > 0 && ry > 0) {
                    std::ostringstream d;
                    d << "M " << -w / 2 + rx << ' ' << -h / 2 << " H " << w / 2 - rx << " A " << rx << ' ' << ry
                      << " 0 0 1 " << w / 2 << ' ' << -h / 2 + ry << " V " << h / 2 - ry << " A " << rx << ' ' << ry
                      << " 0 0 1 " << w / 2 - rx << ' ' << h / 2 << " H " << -w / 2 + rx << " A " << rx << ' ' << ry
                      << " 0 0 1 " << -w / 2 << ' ' << h / 2 - ry << " V " << -h / 2 + ry << " A " << rx << ' ' << ry
                      << " 0 0 1 " << -w / 2 + rx << ' ' << -h / 2 << " Z";
                    p = parsePath(d.str());
                } else
                    p = shapePath("rectangle", w, h);
                for (auto& c : p.contours)
                    for (auto& q : c.points) {
                        q.x += number("x") + w / 2;
                        q.y += number("y") + h / 2;
                    }
            } else if (tag == "ellipse" || tag == "circle") {
                double rx = tag == "circle" ? number("r") : number("rx"), ry = tag == "circle" ? rx : number("ry");
                p = shapePath("ellipse", rx * 2, ry * 2);
                for (auto& c : p.contours)
                    for (auto& q : c.points) {
                        q.x += number("cx");
                        q.y += number("cy");
                    }
            } else if (tag == "line")
                p.contours.push_back({{{number("x1"), number("y1")}, {number("x2"), number("y2")}}, false});
            else if (tag == "polyline" || tag == "polygon") {
                Reader r(a["points"]);
                PathContour c;
                c.closed = tag == "polygon";
                while (!r.end()) {
                    double x = r.num(), y = r.num();
                    c.points.push_back({x, y});
                    if (c.points.size() > maxPoints) fail("too many polygon points");
                }
                if (c.points.size() < 2) fail("SVG polygon needs points");
                p.contours.push_back(c);
            } else
                fail("unsupported SVG element: " + tag);
            for (auto& c : p.contours)
                for (auto& q : c.points) {
                    if (++points > maxPoints) fail("SVG has too many points");
                    q = {m[0] * q.x + m[2] * q.y + m[4] - cx, m[1] * q.x + m[3] * q.y + m[5] - cy};
                    if (!std::isfinite(q.x) || !std::isfinite(q.y) || std::abs(q.x) > 1e8 || std::abs(q.y) > 1e8)
                        fail("excessive SVG transform");
                }
            ImportedPath item;
            item.name = a.contains("id") ? a["id"] : tag + std::to_string(result.size() + 1);
            item.d = pathData(p);
            if (inherited.contains("fill")) item.fill = paint(inherited["fill"]);
            if (inherited.contains("stroke")) item.stroke = paint(inherited["stroke"]);
            if (inherited.contains("stroke-width")) item.strokeWidth = scalar(inherited["stroke-width"]);
            if (item.strokeWidth < 0) fail("negative stroke width");
            if (inherited.contains("fill-opacity"))
                item.fill.a *= std::clamp(scalar(inherited["fill-opacity"]), 0., 1.);
            if (inherited.contains("stroke-opacity"))
                item.stroke.a *= std::clamp(scalar(inherited["stroke-opacity"]), 0., 1.);
            if (inherited.contains("fill-rule")) item.fillRule = inherited["fill-rule"];
            if (item.fillRule != "nonzero" && item.fillRule != "evenodd") fail("unsupported SVG fill rule");
            double sx = std::hypot(m[0], m[1]), sy = std::hypot(m[2], m[3]);
            if (item.stroke.a > 0 && (std::abs(sx - sy) > 1e-6 * std::max({1., sx, sy}) ||
                                      std::abs(m[0] * m[2] + m[1] * m[3]) > 1e-6 * std::max(1., sx * sy)))
                fail("SVG stroked transforms require uniform scale");
            item.strokeWidth *= sx;
            item.linecap = inherited.contains("stroke-linecap") ? inherited["stroke-linecap"] : "butt";
            item.linejoin = inherited.contains("stroke-linejoin") ? inherited["stroke-linejoin"] : "miter";
            item.miterLimit = inherited.contains("stroke-miterlimit") ? scalar(inherited["stroke-miterlimit"]) : 4;
            if (item.linecap != "butt" && item.linecap != "round" && item.linecap != "square")
                fail("unsupported SVG stroke cap");
            if (item.linejoin != "miter" && item.linejoin != "round" && item.linejoin != "bevel")
                fail("unsupported SVG stroke join");
            if (item.miterLimit < 1 || item.miterLimit > 100) fail("SVG miter limit must be 1..100");
            if (a.contains("opacity")) {
                double opacity = std::clamp(scalar(a["opacity"]), 0., 1.);
                if (item.fill.a > 0 && item.stroke.a > 0 && opacity != 1)
                    fail("SVG combined fill/stroke opacity is unsupported; use fill-opacity/stroke-opacity");
                item.fill.a *= opacity;
                item.stroke.a *= opacity;
            }
            if (item.d.size() > 1024 * 1024) fail("flattened SVG path exceeds 1 MiB");
            result.push_back(std::move(item));
        }
    };
    walk(root, viewport, {}, 0);
    if (result.empty()) fail("SVG contains no supported geometry");
    return result;
}
}  // namespace mf
