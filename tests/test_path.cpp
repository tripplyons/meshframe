#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

#include "render/path.hpp"
using namespace mf;
namespace {
struct PathFile {
    std::filesystem::path path;
    explicit PathFile(std::string text, std::string ext = ".svg") {
        static unsigned seq = 0;
        path = std::filesystem::temp_directory_path() /
               ("mf-path-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(++seq) + ext);
        std::ofstream(path) << text;
    }
    ~PathFile() { std::filesystem::remove(path); }
};
}  // namespace
TEST(Path, ParsesAllSvgCommandsAndRoundTrips) {
    auto p = parsePath("M0 0h10v10l-10 0z M20 20 c0 5 5 5 5 0 s5 -5 5 0 q5 10 10 0 t10 0 a5 5 0 0 1 10 0");
    ASSERT_EQ(p.contours.size(), 2u);
    EXPECT_TRUE(p.contours[0].closed);
    EXPECT_FALSE(p.contours[1].closed);
    auto end = p.contours[1].points.back();
    EXPECT_NEAR(end.x, 60, 1e-6);
    EXPECT_NEAR(end.y, 20, 1e-6);
    auto again = parsePath(pathData(p));
    ASSERT_EQ(again.contours.size(), 2u);
    EXPECT_EQ(again.contours[1].points.size(), p.contours[1].points.size());
}
TEST(Path, RejectsMalformedAndExcessiveInput) {
    for (auto s : {"", "L1 2", "M1", "M0 0 C1 2", "M0 0 A1 1 0 2 0 5 5", "Mnan 1", "M0 0 X1 2", "M1e20 0"})
        EXPECT_THROW(parsePath(s), std::runtime_error) << s;
    EXPECT_THROW(shapePath("rectangle", -1, 10), std::runtime_error);
}
TEST(Path, SamplesDistanceAndMorphsDifferentPointCounts) {
    auto p = parsePath("M0 0 L10 0 L10 30");
    auto q = pointOnPath(p, .5);
    EXPECT_NEAR(q.x, 10, 1e-6);
    EXPECT_NEAR(q.y, 10, 1e-6);
    EXPECT_NEAR(pathAngle(p, .5), 90, 1e-5);
    auto morph = morphPath(shapePath("rectangle", 40, 20), shapePath("ellipse", 60, 30), .5);
    ASSERT_EQ(morph.contours.size(), 1u);
    EXPECT_TRUE(morph.contours[0].closed);
    EXPECT_EQ(morph.contours[0].points.size(), 128u);
    EXPECT_THROW(morphPath(p, shapePath("rectangle", 10, 10), .5), std::runtime_error);
    EXPECT_THROW(morphPath(p, parsePath("M0 0L1 1M2 2L3 3"), .5), std::runtime_error);
}
TEST(Path, CompoundFillHonorsWindingAndEvenOdd) {
    auto p = parsePath("M-20 -20L20 -20L20 20L-20 20Z M-10 -10L10 -10L10 10L-10 10Z");
    Image nonzero(64, 64), evenodd(64, 64);
    drawPath(nonzero, p, 1, {1, 0, 0, 1}, {}, 0, 0, 1, "nonzero");
    drawPath(evenodd, p, 1, {1, 0, 0, 1}, {}, 0, 0, 1, "evenodd");
    EXPECT_EQ(nonzero.at(32, 32)[3], 255);
    EXPECT_EQ(evenodd.at(32, 32)[3], 0);
    EXPECT_EQ(evenodd.at(16, 32)[3], 255);
    auto hole = parsePath("M-20 -20L20 -20L20 20L-20 20Z M-10 -10L-10 10L10 10L10 -10Z");
    Image reversed(64, 64);
    drawPath(reversed, hole, 1, {1, 0, 0, 1}, {}, 0, 0, 1, "nonzero");
    EXPECT_EQ(reversed.at(32, 32)[3], 0);
}
TEST(Path, StrokeRevealAndAntialias) {
    Image img(64, 64);
    drawPath(img, parsePath("M-20 0L20 0"), 1, {0, 0, 0, 0}, {0, 1, 0, 1}, 3, 0, .5, "nonzero");
    EXPECT_GT(img.at(20, 32)[3], 200);
    EXPECT_EQ(img.at(45, 32)[3], 0);
    EXPECT_GT(img.at(20, 33)[3], 0);
    EXPECT_LT(img.at(20, 33)[3], 255);
    Image fill(64, 64);
    drawPath(fill, shapePath("rectangle", 40, 40), 1, {1, 0, 0, 1}, {0, 0, 0, 0}, 0, 0, .5, "nonzero");
    EXPECT_EQ(fill.at(32, 32)[3], 0);
}
TEST(Path, SvgImportsInheritedStyleTransformsAndCommonCenter) {
    PathFile f(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="10 20 100 100"><g transform="translate(10,5)" fill="#ff0000" fill-rule="evenodd"><path id="ring" d="M10 20L30 20L30 40Z"/><rect id="box" x="30" y="30" width="10" height="20"/></g></svg>)SVG");
    auto paths = importPaths(f.path.string());
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(paths[0].name, "ring");
    EXPECT_FLOAT_EQ(paths[0].fill.r, 1);
    EXPECT_EQ(paths[0].fillRule, "evenodd");
    auto p = parsePath(paths[0].d);
    EXPECT_NEAR(p.contours[0].points[0].x, -40, 1e-6);
    EXPECT_NEAR(p.contours[0].points[0].y, -45, 1e-6);
}
TEST(Path, SvgImportsPrimitivesAndRawData) {
    PathFile f(
        R"SVG(<svg width="100" height="100"><circle cx="20" cy="20" r="10"/><ellipse cx="30" cy="40" rx="5" ry="10"/><polygon points="0,0 10,0 10,10"/><polyline points="20,20 25,25"/><line x1="0" x2="50" y1="10" y2="10" stroke="#00f" stroke-linecap="round" stroke-linejoin="round"/></svg>)SVG");
    auto p = importPaths(f.path.string());
    ASSERT_EQ(p.size(), 5u);
    EXPECT_NEAR(p.back().stroke.b, 1, 1e-6);
    PathFile raw("M-10 0 C0 -20 10 -20 20 0", ".path");
    EXPECT_EQ(importPaths(raw.path.string()).size(), 1u);
}
TEST(Path, SvgRejectsUnsupportedVisualsAndXml) {
    for (auto body :
         {"<text>Hi</text>", "<use href='#x'/>", "<path d='M0 0L1 1' fill='url(#g)'/>",
          "<rect width='10' height='10' filter='url(#f)'/>", "<g opacity='.5'><rect width='10' height='10'/></g>",
          "<rect width='10' height='10' style='unknown:yes'/>"}) {
        PathFile f(std::string("<svg>") + body + "</svg>");
        EXPECT_THROW(importPaths(f.path.string()), std::runtime_error) << body;
    }
    PathFile bad("<svg><path></svg>");
    EXPECT_THROW(importPaths(bad.path.string()), std::runtime_error);
    PathFile entity("<!DOCTYPE svg [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><svg><title>&x;</title></svg>");
    EXPECT_THROW(importPaths(entity.path.string()), std::runtime_error);
}
TEST(Path, CloseFollowedByDrawStartsOpenContour) {
    auto p = parsePath("M0 0L10 0L10 10Z L20 0 l0 10");
    ASSERT_EQ(p.contours.size(), 2u);
    EXPECT_TRUE(p.contours[0].closed);
    EXPECT_FALSE(p.contours[1].closed);
    EXPECT_DOUBLE_EQ(p.contours[1].points.front().x, 0);
    EXPECT_DOUBLE_EQ(p.contours[1].points.back().x, 20);
    auto q = parsePath("M0 0L10 0Z c0 5 5 5 5 0");
    ASSERT_EQ(q.contours.size(), 2u);
    EXPECT_FALSE(q.contours[1].closed);
}
TEST(Path, CompactArcFlagsAndExtremeRadii) {
    auto p = parsePath("M0 0 A10 10 0 0110 20");
    EXPECT_NEAR(p.contours[0].points.back().x, 10, 1e-6);
    EXPECT_THROW(parsePath("M0 0 A1e-320 1e-320 0 0 1 10 10"), std::runtime_error);
    auto tiny = parsePath("M0 0 A1e-300 1e-300 0 0 1 10 10");
    EXPECT_NEAR(tiny.contours[0].points.back().x, 10, 1e-6);
    Image image(32, 32);
    EXPECT_THROW(drawPath(image, shapePath("line", 20, 0), 1, {}, {1, 1, 1, 1}, 2,
                          std::numeric_limits<double>::quiet_NaN(), 1, "nonzero"),
                 std::runtime_error);
}
TEST(Path, SvgDefaultsAndRoundedRectangle) {
    PathFile f("<svg><rect width='20' height='10' rx='2' stroke='#fff'/><line x2='10' stroke='#fff'/></svg>");
    auto p = importPaths(f.path.string());
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(p[0].linecap, "butt");
    EXPECT_EQ(p[0].linejoin, "miter");
    EXPECT_GT(parsePath(p[0].d).contours[0].points.size(), 4u);
}
TEST(Path, StrokeCapAndJoinGeometry) {
    auto line = parsePath("M-10 0L10 0");
    Image butt(64, 64), square(64, 64);
    drawPath(butt, line, 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 8, 0, 1, "nonzero", "butt", "miter");
    drawPath(square, line, 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 8, 0, 1, "nonzero", "square", "miter");
    EXPECT_EQ(butt.at(44, 32)[3], 0);
    EXPECT_GT(square.at(44, 32)[3], 200);
    auto corner = parsePath("M-10 10 L0 0 L10 10");
    Image miter(64, 64), bevel(64, 64);
    drawPath(miter, corner, 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 10, 0, 1, "nonzero", "butt", "miter");
    drawPath(bevel, corner, 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 10, 0, 1, "nonzero", "butt", "bevel");
    EXPECT_GT(miter.at(32, 26)[3], bevel.at(32, 26)[3]);
}

TEST(Path, SvgViewportMappingAndAlignment) {
    PathFile scaled(
        "<svg width='200' height='200' viewBox='10 20 100 100'><rect x='10' y='20' width='100' height='100'/></svg>");
    auto p = parsePath(importPaths(scaled.path.string())[0].d);
    EXPECT_NEAR(p.contours[0].points[0].x, -100, 1e-6);
    EXPECT_NEAR(p.contours[0].points[2].x, 100, 1e-6);
    PathFile meet(
        "<svg width='200' height='100' viewBox='0 0 100 100' preserveAspectRatio='xMaxYMin meet'><rect width='100' "
        "height='100'/></svg>");
    auto m = parsePath(importPaths(meet.path.string())[0].d);
    EXPECT_NEAR(m.contours[0].points[0].x, 0, 1e-6);
    EXPECT_NEAR(m.contours[0].points[2].x, 100, 1e-6);
    PathFile none(
        "<svg width='200' height='100' viewBox='0 0 100 100' preserveAspectRatio='none'><rect width='100' "
        "height='100'/></svg>");
    auto n = parsePath(importPaths(none.path.string())[0].d);
    EXPECT_NEAR(n.contours[0].points[0].x, -100, 1e-6);
    EXPECT_NEAR(n.contours[0].points[2].y, 50, 1e-6);
    PathFile bad("<svg viewBox='0 0 100 100' preserveAspectRatio='invalid'><rect width='10' height='10'/></svg>");
    EXPECT_THROW(importPaths(bad.path.string()), std::runtime_error);
}

TEST(Path, TrimBoundaryCapsAndDuplicateClosure) {
    auto corner = parsePath("M-20 0L0 0L0 20");
    for (auto cap : {"round", "square"}) {
        Image ending(64, 64), starting(64, 64);
        drawPath(ending, corner, 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 8, 0, .5, "nonzero", cap, "miter");
        EXPECT_GT(ending.at(34, 32)[3], 200) << cap;
        drawPath(starting, corner, 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 8, .5, 1, "nonzero", cap, "miter");
        EXPECT_GT(starting.at(32, 29)[3], 200) << cap;
    }
    for (auto join : {"round", "miter"}) {
        Image implicit(64, 64), explicitClose(64, 64);
        drawPath(implicit, parsePath("M0 0L20 0L20 20Z"), 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 8, 0, 1, "nonzero", "round",
                 join);
        drawPath(explicitClose, parsePath("M0 0L20 0L20 20L0 0Z"), 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 8, 0, 1, "nonzero",
                 "round", join);
        EXPECT_EQ(implicit.px, explicitClose.px) << join;
    }
    Image partial(64, 64);
    drawPath(partial, parsePath("M0 0L20 0L20 20Z"), 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 8, 0, .2, "nonzero", "round",
             "miter");
    EXPECT_GT(partial.at(29, 32)[3], 200);
}
