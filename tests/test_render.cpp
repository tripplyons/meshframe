#include "engine/frame_renderer.hpp"
#include "fixtures.hpp"
#include "media/decoder.hpp"
#include "model/edit.hpp"
#include "render/compositor.hpp"
#include "render/raster.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

using namespace mf;

namespace {

struct Px {
    int r, g, b, a;
};

Px px(const Image& img, int x, int y) {
    const uint8_t* p = img.at(x, y);
    return {p[0], p[1], p[2], p[3]};
}

#define EXPECT_PX(img, x, y, R, G, B, tol)                  \
    do {                                                    \
        Px p_ = px(img, x, y);                              \
        EXPECT_NEAR(p_.r, R, tol) << "at " << x << "," << y; \
        EXPECT_NEAR(p_.g, G, tol) << "at " << x << "," << y; \
        EXPECT_NEAR(p_.b, B, tol) << "at " << x << "," << y; \
    } while (0)

Project smallProject() {
    Project p = Project::makeDefault();
    p.script = "";
    p.settings.width = 64;
    p.settings.height = 32;
    return p;
}

Id addSolid(Project& p, const char* track, double start, double dur, Color c) {
    Clip clip;
    clip.kind = ClipKind::Solid;
    clip.track = p.findTrackByName(track)->id;
    clip.start = start;
    clip.duration = dur;
    Id id = edit::addClip(p, clip);
    edit::setPropConstant(p, id, "color", c);
    return id;
}

struct Fixture {
    MediaCache cache;
    FrameRenderer renderer{cache};
    Evaluator ev;
    std::shared_ptr<Image> render(const Project& p, double t, int w = 64, int h = 32) {
        ev.setProject(p);
        return renderer.render(ev, t, w, h);
    }
};

const Color red{1, 0, 0, 1}, blue{0, 0, 1, 1}, white = Color::gray(1);

}  // namespace

TEST(Render, PolygonCoverageIsExactOnPixelEdges) {
    Image img(8, 4);
    Vec2 pts[4] = {{1.5f, 0}, {3.5f, 0}, {3.5f, 4}, {1.5f, 4}};
    fillPolygon(img, pts, white);
    EXPECT_EQ(px(img, 0, 1).a, 0);
    EXPECT_NEAR(px(img, 1, 1).a, 128, 1);
    EXPECT_EQ(px(img, 2, 1).a, 255);
    EXPECT_NEAR(px(img, 3, 1).a, 128, 1);
    EXPECT_EQ(px(img, 4, 1).a, 0);
}

TEST(Render, SolidTransformOpacityAndBackground) {
    Project p = smallProject();
    p.settings.background = Color::gray(0.0f);
    Id id = addSolid(p, "V1", 0, 2, red);
    edit::setPropConstant(p, id, "scale", 0.5);
    edit::setPropConstant(p, id, "x", 8.0);
    Fixture f;
    auto img = f.render(p, 1);
    EXPECT_PX(*img, 40, 16, 255, 0, 0, 1);  // center shifted right by 8
    EXPECT_PX(*img, 2, 2, 0, 0, 0, 0);       // outside the half-size layer
    EXPECT_PX(*img, 20, 16, 0, 0, 0, 0);

    edit::setPropConstant(p, id, "opacity", 0.5);
    img = f.render(p, 1);
    EXPECT_PX(*img, 40, 16, 128, 0, 0, 1);

    // Outside the clip's time range, only the background shows.
    img = f.render(p, 3);
    EXPECT_PX(*img, 40, 16, 0, 0, 0, 0);
}

TEST(Render, PreviewScaleUsesProjectUnits) {
    Project p = smallProject();
    Id id = addSolid(p, "V1", 0, 2, red);
    edit::setPropConstant(p, id, "scale", 0.5);
    edit::setPropConstant(p, id, "x", 16.0);
    Fixture f;
    auto img = f.render(p, 0, 32, 16);  // half size: x offset becomes 8 output pixels
    EXPECT_EQ(img->w, 32);
    EXPECT_PX(*img, 24, 8, 255, 0, 0, 1);
    EXPECT_PX(*img, 13, 8, 0, 0, 0, 0);
}

TEST(Render, RotationAndHigherTracksDrawOnTop) {
    Project p = smallProject();
    addSolid(p, "V1", 0, 2, red);
    Id top = addSolid(p, "V2", 0, 2, blue);
    edit::setPropConstant(p, top, "scale", 0.25);
    edit::setPropConstant(p, top, "rotation", 45.0);
    Fixture f;
    auto img = f.render(p, 0);
    EXPECT_PX(*img, 32, 16, 0, 0, 255, 1);
    EXPECT_PX(*img, 2, 2, 255, 0, 0, 1);
    // A 16x8 box rotated 45 degrees reaches about 5.7 px from center along x.
    EXPECT_PX(*img, 32 + 4, 16, 0, 0, 255, 1);
    EXPECT_PX(*img, 32 + 8, 16, 255, 0, 0, 1);
}

TEST(Render, BlendModes) {
    Project p = smallProject();
    addSolid(p, "V1", 0, 2, Color::gray(0.5f));
    Id top = addSolid(p, "V2", 0, 2, Color::gray(0.5f));
    Fixture f;
    edit::setPropConstant(p, top, "blend", std::string("add"));
    EXPECT_PX(*f.render(p, 0), 5, 5, 255, 255, 255, 1);
    edit::setPropConstant(p, top, "blend", std::string("multiply"));
    EXPECT_PX(*f.render(p, 0), 5, 5, 64, 64, 64, 1);
    edit::setPropConstant(p, top, "blend", std::string("screen"));
    EXPECT_PX(*f.render(p, 0), 5, 5, 191, 191, 191, 1);
}

TEST(Render, AlignedOpaqueLayerCopiesEveryChannelValueAndClips) {
    Image src(256, 4);
    for (int y = 0; y < src.h; ++y) for (int x = 0; x < src.w; ++x) {
        uint8_t* s = src.at(x, y);
        s[0] = uint8_t(x);
        s[1] = uint8_t(255 - x);
        s[2] = uint8_t(x * 73 + y);
        s[3] = 255;
    }
    for (auto [left, top] : {std::pair{0, 0}, {-43, -2}, {17, 5}, {400, 0}}) {
        Image actual(279, 8);
        fill(actual, Color{0.2f, 0.4f, 0.6f, 0.5f});
        Image expected = actual;
        for (int y = 0; y < src.h; ++y) for (int x = 0; x < src.w; ++x) {
            if (x + left >= 0 && x + left < actual.w && y + top >= 0 && y + top < actual.h)
                std::copy_n(src.at(x, y), 4, expected.at(x + left, y + top));
        }
        drawLayer(actual, src, Placement{left + src.w / 2.0, top + src.h / 2.0}, 1, Blend::Normal);
        EXPECT_EQ(actual.px, expected.px);
    }
}

TEST(Render, AlignedLayerBlendsAlphaOpacityAndModes) {
    const std::array<uint8_t, 4> alpha{0, 64, 128, 255};
    Image src(4, 1);
    for (int x = 0; x < src.w; ++x) {
        src.at(x, 0)[0] = alpha[x];
        src.at(x, 0)[3] = alpha[x];
    }
    for (float opacity : {0.0f, 0.5f, 1.0f}) {
        for (Blend blend : {Blend::Normal, Blend::Add, Blend::Multiply, Blend::Screen}) {
            Image dst(4, 1);
            fill(dst, Color{0, 0, 1, 1});
            drawLayer(dst, src, Placement{2, 0.5}, opacity, blend);
            for (int x = 0; x < dst.w; ++x) {
                int strength = int(std::lround(alpha[x] * opacity));
                int remaining = int(std::lround(255.0f - alpha[x] * opacity));
                EXPECT_EQ(px(dst, x, 0).r, blend == Blend::Multiply ? 0 : strength);
                EXPECT_EQ(px(dst, x, 0).g, 0);
                EXPECT_EQ(px(dst, x, 0).b, blend == Blend::Normal || blend == Blend::Multiply ? remaining : 255);
                EXPECT_EQ(px(dst, x, 0).a, 255);
            }
        }
    }
}

TEST(Render, ImageStorageSurvivesCopiesMovesAndResizes) {
    Image original(65, 33);
    for (size_t i = 0; i < original.px.size(); ++i) original.px[i] = uint8_t(i * 73);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(original.px.data()) % detail::pixelAlignment(), 0u);
    Image copy = original;
    EXPECT_EQ(copy.px, original.px);
    EXPECT_NE(copy.px.data(), original.px.data());
    Image moved = std::move(copy);
    EXPECT_EQ(moved.px, original.px);
    moved.resize(257, 127);
    EXPECT_EQ(moved.bytes(), size_t(257 * 127 * 4));
    EXPECT_TRUE(std::ranges::all_of(moved.px, [](uint8_t v) { return v == 0; }));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(moved.px.data()) % detail::pixelAlignment(), 0u);
    moved.resize(1, 1);
    EXPECT_EQ(moved.px.size(), 4u);
    EXPECT_TRUE(std::ranges::all_of(moved.px, [](uint8_t v) { return v == 0; }));
}

TEST(Render, NegativeScaleMirrorsLayersAndSolidFallback) {
    Image source(8, 4), actual(8, 4);
    for (int y = 0; y < source.h; ++y)
        for (int x = 0; x < source.w; ++x) {
            auto* p = source.at(x, y);
            p[0] = uint8_t(x * 30);
            p[1] = uint8_t(y * 60);
            p[3] = 255;
        }
    drawLayer(actual, source, Placement{4, 2, -1, -1}, 1, Blend::Normal);
    for (int y = 0; y < actual.h; ++y)
        for (int x = 0; x < actual.w; ++x)
            for (int c = 0; c < 4; ++c) EXPECT_EQ(actual.at(x, y)[c], source.at(7 - x, 3 - y)[c]);
    EXPECT_FALSE(drawSolidLayer(actual, Placement{4, 2, -1, 1}, 8, 4, Color{1, 0, 0, 1}, 1, Blend::Normal));
}

TEST(Render, RecycledImageStorageIsZeroedAndIndependentAcrossThreads) {
    std::array<std::thread, 4> workers;
    for (size_t worker = 0; worker < workers.size(); ++worker) {
        workers[worker] = std::thread([worker] {
            for (int round = 0; round < 32; ++round) {
                Image retained(257, 127);
                const uint8_t value = uint8_t(worker * 32 + round + 1);
                std::fill(retained.px.begin(), retained.px.end(), value);
                for (int reuse = 0; reuse < 4; ++reuse) {
                    Image image(257 + round % 3, 127);
                    EXPECT_TRUE(std::ranges::all_of(image.px, [](uint8_t v) { return v == 0; }));
                    std::fill(image.px.begin(), image.px.end(), uint8_t(value + reuse));
                }
                EXPECT_TRUE(std::ranges::all_of(retained.px, [value](uint8_t v) { return v == value; }));
            }
        });
    }
    for (auto& worker : workers) worker.join();
}

TEST(Render, VideoEffects) {
    Fixture f;
    auto with = [&](Color c, const char* type, std::vector<std::pair<const char*, Value>> props) {
        Project p = smallProject();
        Id id = addSolid(p, "V1", 0, 2, c);
        Id fx = edit::addEffect(p, id, false, type);
        for (auto& [k, v] : props) edit::setPropConstant(p, fx, k, v);
        return f.render(p, 0);
    };
    EXPECT_PX(*with(white, "invert", {}), 5, 5, 0, 0, 0, 0);
    EXPECT_PX(*with(red, "saturation", {{"amount", 0.0}}), 5, 5, 54, 54, 54, 1);
    EXPECT_PX(*with(Color::gray(0.5f), "brightness_contrast", {{"brightness", 0.25}}), 5, 5, 191, 191, 191, 1);
    EXPECT_PX(*with(Color::gray(0.75f), "brightness_contrast", {{"contrast", 2.0}}), 5, 5, 255, 255, 255, 1);
    EXPECT_PX(*with(Color::gray(0.6f), "threshold", {{"level", 0.5}}), 5, 5, 255, 255, 255, 0);
    EXPECT_PX(*with(Color::gray(0.4f), "threshold", {{"level", 0.5}}), 5, 5, 0, 0, 0, 0);
    EXPECT_PX(*with(white, "tint", {{"color", blue}, {"amount", 1.0}}), 5, 5, 0, 0, 255, 1);
    auto vig = with(white, "vignette", {{"amount", 1.0}, {"softness", 0.5}});
    EXPECT_PX(*vig, 32, 16, 255, 255, 255, 1);
    EXPECT_LT(px(*vig, 0, 0).r, 10);
    // Blur spreads a hard edge inside the layer; layer borders clamp, so full frames do not darken.
    Project p = smallProject();
    p.script = R"(mf.generator("half", function(c) c:clear(gray(0)) c:rect(32, 0, 32, 32, gray(1)) end))";
    Clip gen;
    gen.kind = ClipKind::Generator;
    gen.track = p.findTrackByName("V1")->id;
    gen.duration = 2;
    Id gid = edit::addClip(p, gen);
    edit::setPropConstant(p, gid, "generator", std::string("half"));
    auto sharp = f.render(p, 0);
    EXPECT_EQ(px(*sharp, 30, 16).r, 0);
    EXPECT_EQ(px(*sharp, 33, 16).r, 255);
    Id blurFx = edit::addEffect(p, gid, false, "blur");
    edit::setPropConstant(p, blurFx, "radius", 8.0);
    auto soft = f.render(p, 0);
    EXPECT_GT(px(*soft, 30, 16).r, 20);
    EXPECT_LT(px(*soft, 33, 16).r, 235);
    EXPECT_PX(*soft, 63, 16, 255, 255, 255, 1);
}

TEST(Render, BlurMatchesClampedKernelOnNarrowAndWideImages) {
    for (auto [w, h] : {std::pair{1, 1}, {1, 9}, {11, 1}, {3, 5}, {19, 13}, {67, 33}}) {
        for (int radius : {0, 1, 3, 24}) {
            SCOPED_TRACE(::testing::Message() << w << 'x' << h << " radius " << radius);
            Image actual(w, h);
            uint32_t seed = 17;
            for (auto& c : actual.px) {
                seed = seed * 1664525u + 1013904223u;
                c = uint8_t(seed >> 24);
            }
            Image expected = actual;
            if (radius > 0) {
                const float norm = 1.0f / (2 * radius + 1);
                for (int pass = 0; pass < 6; ++pass) {
                    Image next(w, h);
                    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                        std::array<int, 4> sum{};
                        for (int offset = -radius; offset <= radius; ++offset) {
                            int sx = pass < 3 ? std::clamp(x + offset, 0, w - 1) : x;
                            int sy = pass < 3 ? y : std::clamp(y + offset, 0, h - 1);
                            for (int c = 0; c < 4; ++c) sum[c] += expected.at(sx, sy)[c];
                        }
                        for (int c = 0; c < 4; ++c) next.at(x, y)[c] = uint8_t(sum[c] * norm + 0.5f);
                    }
                    expected = std::move(next);
                }
            }
            applyEffect(actual, "blur", [&](std::string_view name) -> Value {
                return name == "radius" ? Value{double(radius * 2)} : Value{};
            }, 1);
            EXPECT_EQ(actual.px, expected.px);
        }
    }
}

TEST(Render, TransitionsMixAcrossTheCut) {
    Project p = smallProject();
    Id a = addSolid(p, "V1", 0, 2, red);
    Id b = addSolid(p, "V1", 2, 2, blue);
    Id tr = edit::addTransition(p, a, b, "crossfade", 1.0);
    Fixture f;
    // Window is 1.5..2.5; at 2.0 the ease-in-out curve gives a mix of 0.5.
    EXPECT_PX(*f.render(p, 2.0), 5, 5, 128, 0, 128, 2);
    EXPECT_PX(*f.render(p, 1.4), 5, 5, 255, 0, 0, 0);
    EXPECT_PX(*f.render(p, 2.6), 5, 5, 0, 0, 255, 0);
    // Clip A extends past its end inside the window.
    auto late = f.render(p, 2.2);
    EXPECT_GT(px(*late, 5, 5).r, 0);
    EXPECT_GT(px(*late, 5, 5).b, px(*late, 5, 5).r);

    edit::setTransition(p, tr, std::string("wipe"), std::nullopt);
    edit::setPropConstant(p, tr, "softness", 0.0);
    auto wipe = f.render(p, 2.0);  // B covers from the right
    EXPECT_PX(*wipe, 5, 16, 255, 0, 0, 0);
    EXPECT_PX(*wipe, 58, 16, 0, 0, 255, 0);

    edit::setTransition(p, tr, std::string("dip"), std::nullopt);
    EXPECT_PX(*f.render(p, 2.0), 5, 5, 0, 0, 0, 1);

    edit::setTransition(p, tr, std::string("slide"), std::nullopt);
    auto slide = f.render(p, 2.0);  // A pushed half out to the left, B half in from the right
    EXPECT_PX(*slide, 5, 16, 255, 0, 0, 0);
    EXPECT_PX(*slide, 58, 16, 0, 0, 255, 0);
}

TEST(Render, TextAndGeneratorClips) {
    Project p = smallProject();
    p.settings.width = 256;
    p.settings.height = 128;
    Clip text;
    text.kind = ClipKind::Text;
    text.track = p.findTrackByName("V1")->id;
    text.duration = 2;
    Id tid = edit::addClip(p, text);
    edit::setPropConstant(p, tid, "text", std::string("HI"));
    edit::setPropConstant(p, tid, "size", 64.0);
    Fixture f;
    auto img = f.render(p, 0, 256, 128);
    int lit = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 256; ++x) lit += px(*img, x, y).r > 128;
    EXPECT_GT(lit, 200);
    EXPECT_EQ(px(*img, 2, 2).r, 0);
    EXPECT_EQ(px(*img, 128, 10).r, 0);  // text is centered vertically, so the top stays dark

    Project g = smallProject();
    g.script = R"(
mf.generator("box", function(c)
  c:rect(10, 4, 20, 10, rgb(0, 1, 0))
  c:circle(48, 20, 6, "#0000ff")
end)
)";
    Clip gen;
    gen.kind = ClipKind::Generator;
    gen.track = g.findTrackByName("V1")->id;
    gen.duration = 2;
    Id gid = edit::addClip(g, gen);
    edit::setPropConstant(g, gid, "generator", std::string("box"));
    img = f.render(g, 0);
    EXPECT_PX(*img, 15, 8, 0, 255, 0, 0);
    EXPECT_PX(*img, 48, 20, 0, 0, 255, 0);
    EXPECT_PX(*img, 5, 25, 0, 0, 0, 0);
    EXPECT_TRUE(f.ev.errors().empty());

    edit::setPropConstant(g, gid, "generator", std::string("missing"));
    f.render(g, 0);
    ASSERT_FALSE(f.ev.errors().empty());
    EXPECT_NE(f.ev.errors()[0].message.find("missing"), std::string::npos);
}

TEST(Render, MediaClipShowsTheSourceFrame) {
    if (!fixtures::haveFfmpeg()) GTEST_SKIP() << "ffmpeg CLI not installed";
    Project p = smallProject();
    MediaSource m;
    std::string err;
    ASSERT_TRUE(probeMedia(fixtures::countingVideo(), m, &err)) << err;
    Id mid = edit::addMedia(p, m);
    auto ids = edit::addMediaClips(p, mid, 1.0);
    edit::trimClip(p, ids[0], 1.0, 1.0);  // keep the timeline position
    Clip* c = p.findClip(ids[0]);
    c->in = 0.5;
    Fixture f;
    // Timeline 1.3 -> source 0.8 -> frame 8. The 64x48 source fits the 64x32 frame as 43x32.
    auto img = f.render(p, 1.3);
    double y = 16 + px(*img, 32, 16).g * 219.0 / 255.0;
    EXPECT_NEAR((y - 20) / 7, 8, 0.5);
    EXPECT_EQ(px(*img, 2, 16).a, 255);
    EXPECT_PX(*img, 2, 16, 0, 0, 0, 0);  // pillarbox
}
