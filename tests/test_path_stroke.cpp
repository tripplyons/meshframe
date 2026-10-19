#include "render/compositor.hpp"
#include "render/path.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>

using namespace mf;

TEST(PathRender, StrokeScanlinesPreserveTheQuarterPixelSampleGrid) {
    const std::array<std::pair<PathPoint, PathPoint>, 6> lines{{
        {{-20, -10}, {25, 12}}, {{-10, 0}, {10, 0}}, {{0, -12}, {0, 12}},
        {{-12, -12}, {12, 12}}, {{10, -8}, {-10, 8}}, {{-1.125, 0.375}, {1.875, -0.125}}}};
    for (auto [from, to] : lines)
        for (double width : {0.25, 1.0, 2.75, 9.0})
            for (double unit : {0.5, 1.0, 2.0})
                for (float opacity : {0.63f, 1.0f}) {
                    SCOPED_TRACE(::testing::Message() << from.x << ',' << from.y << " to " << to.x << ',' << to.y
                                                     << " width " << width << " unit " << unit << " alpha " << opacity);
                    Image actual(37, 29);
                    fill(actual, Color{0.2f, 0.4f, 0.6f, 0.7f});
                    Image expected = actual;
                    const Color stroke{0.7f, 0.2f, 0.9f, opacity};
                    VectorPath path{{PathContour{{from, to}, false}}};
                    drawPath(actual, path, unit, Color{0, 0, 0, 0}, stroke, width, 0, 1, "nonzero", "butt", "bevel");
                    PathPoint a{from.x * unit + actual.w / 2.0, from.y * unit + actual.h / 2.0};
                    PathPoint b{to.x * unit + actual.w / 2.0, to.y * unit + actual.h / 2.0};
                    double length = std::hypot(b.x - a.x, b.y - a.y), radius = width * unit / 2;
                    double nx = -(b.y - a.y) / length * radius, ny = (b.x - a.x) / length * radius;
                    const std::array<PathPoint, 4> quad{{
                        {a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}}};
                    for (int y = 0; y < actual.h; ++y)
                        for (int x = 0; x < actual.w; ++x) {
                            int covered = 0;
                            for (int sy = 0; sy < 4; ++sy)
                                for (int sx = 0; sx < 4; ++sx) {
                                    double xx = x + (sx + 0.5) / 4, yy = y + (sy + 0.5) / 4;
                                    bool inside = false;
                                    for (size_t j = 0, k = quad.size() - 1; j < quad.size(); k = j++) {
                                        const auto& p = quad[j];
                                        const auto& q = quad[k];
                                        if ((p.y > yy) != (q.y > yy) && xx < (q.x - p.x) * (yy - p.y) / (q.y - p.y) + p.x)
                                            inside = !inside;
                                    }
                                    covered += inside;
                                }
                            if (!covered) continue;
                            double alpha = double(opacity) * covered / 16, inverse = 1 - alpha;
                            uint8_t* pixel = expected.at(x, y);
                            const double color[4] = {stroke.r, stroke.g, stroke.b, 1};
                            for (int channel = 0; channel < 4; ++channel)
                                pixel[channel] = uint8_t(std::clamp(color[channel] * alpha * 255 + pixel[channel] * inverse,
                                                                  0.0, 255.0));
                        }
                    EXPECT_EQ(actual.px, expected.px);
                }
}
