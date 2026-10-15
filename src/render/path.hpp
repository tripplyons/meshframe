#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "core/value.hpp"
#include "render/image.hpp"
namespace mf {
struct PathPoint {
    double x = 0, y = 0;
};
struct PathContour {
    std::vector<PathPoint> points;
    bool closed = false;
};
struct VectorPath {
    std::vector<PathContour> contours;
};
// SVG path syntax. Curves are flattened to local-coordinate polylines.
VectorPath parsePath(std::string_view d);
VectorPath shapePath(std::string_view shape, double width, double height);
VectorPath morphPath(const VectorPath& a, const VectorPath& b, double progress);
PathPoint pointOnPath(const VectorPath& path, double progress);
double pathAngle(const VectorPath& path, double progress);
std::string pathData(const VectorPath& path);
struct ImportedPath {
    std::string name, d;
    Color fill{0, 0, 0, 1}, stroke{0, 0, 0, 0};
    double strokeWidth = 1;
    std::string fillRule = "nonzero";
    std::string linecap = "round", linejoin = "round";
    double miterLimit = 4;
};
std::vector<ImportedPath> importPaths(const std::string& filename);
// Coordinates use the image center as origin; unit scales project units to pixels.
void drawPath(Image& dst, const VectorPath& path, double unit, Color fill, Color stroke, double strokeWidth,
              double trimStart, double trimEnd, std::string_view fillRule, std::string_view linecap = "round",
              std::string_view linejoin = "round", double miterLimit = 4);
}  // namespace mf
