#include "render/compositor.hpp"
#include "render/path.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace mf;

namespace {
// Reference each subrow against every contour edge, independently of the
// renderer's active-edge sweep, then integrate horizontal coverage exactly.
void referenceFill(Image& dst, VectorPath path, double unit, Color color, bool evenodd) {
    for (auto& contour : path.contours)
        for (auto& point : contour.points)
            point = {point.x * unit + dst.w / 2.0, point.y * unit + dst.h / 2.0};
    std::vector<double> coverage(dst.w);
    std::vector<std::pair<double, int>> hits;
    for (int y = 0; y < dst.h; ++y) {
        std::fill(coverage.begin(), coverage.end(), 0);
        for (int sub = 0; sub < 4; ++sub) {
            double yy = y + (sub + .5) / 4;
            hits.clear();
            for (const auto& contour : path.contours) {
                if (contour.points.size() < 3) continue;
                for (size_t i = 0; i < contour.points.size(); ++i) {
                    auto a = contour.points[i], b = contour.points[(i + 1) % contour.points.size()];
                    if ((a.y <= yy && b.y > yy) || (b.y <= yy && a.y > yy))
                        hits.push_back({a.x + (yy - a.y) * (b.x - a.x) / (b.y - a.y), b.y > a.y ? 1 : -1});
                }
            }
            std::sort(hits.begin(), hits.end());
            int winding = 0;
            double left = 0;
            for (auto [x, direction] : hits) {
                bool before = evenodd ? winding % 2 != 0 : winding != 0;
                winding += direction;
                bool after = evenodd ? winding % 2 != 0 : winding != 0;
                if (!before && after) left = x;
                if (before && !after) {
                    double l = std::clamp(left, 0.0, double(dst.w)), r = std::clamp(x, 0.0, double(dst.w));
                    for (int column = int(std::floor(l)); column < std::min(dst.w, int(std::ceil(r))); ++column)
                        coverage[column] += std::max(0.0, std::min(r, double(column + 1)) - std::max(l, double(column))) / 4;
                }
            }
        }
        for (int x = 0; x < dst.w; ++x) {
            if (coverage[x] <= 0) continue;
            double alpha = std::clamp(double(color.a) * coverage[x], 0.0, 1.0), inverse = 1 - alpha;
            auto* pixel = dst.at(x, y);
            const double channels[4] = {color.r, color.g, color.b, 1};
            for (int c = 0; c < 4; ++c)
                pixel[c] = uint8_t(std::clamp(channels[c] * alpha * 255 + pixel[c] * inverse, 0.0, 255.0));
        }
    }
}
}  // namespace

TEST(PathRender, ActiveFillEdgesPreserveCoverageWindingAndClipping) {
    const std::array<const char*, 8> paths{{
        "M-30-10L25-10L25 11L-30 11Z",
        "M-13-9L13-9L13 9L-13 9Z M-6-4L6-4L6 4L-6 4Z",
        "M-13-9L13-9L13 9L-13 9Z M-6-4L-6 4L6 4L6-4Z",
        "M-17-10L16 11L-15 11L16-10Z",
        "M0-12L3-3L14-3L5 3L9 13L0 7L-9 13L-5 3L-14-3L-3-3Z",
        "M-12-0.375L0-0.375L0 0.125L12 0.125L-3 9.625Z",
        "M-100-100L-80-100L-80-80Z M0 0L1 1",
        "M-15-10C-20 18 20-18 15 10L-15 10Z"}};
    for (const char* data : paths)
        for (double unit : {.5, 1.0, 2.0})
            for (const char* rule : {"nonzero", "evenodd"})
                for (float opacity : {.63f, 1.0f}) {
                    SCOPED_TRACE(::testing::Message() << data << " unit " << unit << " rule " << rule << " alpha " << opacity);
                    auto path = parsePath(data);
                    Image actual(37, 29);
                    fill(actual, Color{.2f, .4f, .6f, .7f});
                    Image expected = actual;
                    Color color{.7f, .2f, .9f, opacity};
                    referenceFill(expected, path, unit, color, std::string_view(rule) == "evenodd");
                    drawPath(actual, path, unit, color, Color{}, 0, 0, 1, rule);
                    EXPECT_EQ(actual.px, expected.px);
                }
}
