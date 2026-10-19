#include "engine/frame_renderer.hpp"
#include "model/edit.hpp"
#include "render/gpu.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>

using namespace mf;

namespace {

std::shared_ptr<Image> pattern(int w, int h) {
    auto image = std::make_shared<Image>(w, h);
    uint32_t seed = 17;
    for (size_t i = 0; i < image->px.size(); i += 4) {
        seed = seed * 1664525u + 1013904223u;
        uint8_t a = uint8_t(seed >> 24);
        image->px[i] = uint8_t((seed >> 16) % (a + 1));
        image->px[i + 1] = uint8_t((seed >> 8) % (a + 1));
        image->px[i + 2] = uint8_t(seed % (a + 1));
        image->px[i + 3] = a;
    }
    return image;
}

void expectNear(const Image& actual, const Image& expected, int tolerance = 1) {
    ASSERT_EQ(actual.w, expected.w);
    ASSERT_EQ(actual.h, expected.h);
    int worst = 0;
    for (size_t i = 0; i < actual.px.size(); ++i) worst = std::max(worst, std::abs(int(actual.px[i]) - expected.px[i]));
    EXPECT_LE(worst, tolerance);
}

} // namespace

class GpuRendering : public ::testing::Test {
    void SetUp() override {
        if (gpuAvailable()) return;
        const std::string error = gpuError();
        if (error == "no Metal device" || error == "Metal rendering is not enabled in this build")
            GTEST_SKIP() << error;
        FAIL() << error;
    }
};

TEST_F(GpuRendering, TransformsBlendModesAndTransparentEdgesMatchCpu) {
    auto src = pattern(13, 9);
    for (const Placement& at : {Placement{15.5, 11.5},
                                {12.2, 10.7, 0.73, 1.6, 0},
                                {16.1, 8.9, 1.4, 0.8, 0.31},
                                {2.1, 3.2, -1.2, 1.3, 0.6},
                                {15.5, 11.5, -1, -1}}) {
        for (float opacity : {0.0f, 0.5f, 1.0f})
            for (Blend blend : {Blend::Normal, Blend::Add, Blend::Multiply, Blend::Screen}) {
                auto dst = std::make_shared<Image>(31, 23);
                Image expected(31, 23);
                const Color background{0.2f, 0.4f, 0.7f, 0.8f};
                fill(expected, background);
                drawLayer(expected, *src, at, opacity, blend);
                auto gpu = createGpuFrame();
                ASSERT_TRUE(gpu);
                gpu->fill(dst, background);
                gpu->drawLayer(dst, src, at, opacity, blend);
                ASSERT_TRUE(gpu->finish(dst));
                expectNear(*dst, expected);
            }
    }
}

TEST_F(GpuRendering, KeepsSourcesAndIntermediateSurfacesUntilCompletion) {
    auto gpu = createGpuFrame();
    auto dst = std::make_shared<Image>(32, 24);
    Image expected(32, 24);
    gpu->fill(dst, Color{0, 0, 0, 0});
    for (int i = 0; i < 9; ++i) {
        auto src = pattern(8, 6);
        const Placement at{4.0 + i * 2, 3.0 + i};
        drawLayer(expected, *src, at, 0.7f, Blend::Normal);
        gpu->drawLayer(dst, src, at, 0.7f, Blend::Normal);
    }
    auto next = std::make_shared<Image>(32, 24);
    gpu->fill(next, Color{0, 0, 0, 0});
    gpu->drawLayer(next, dst, Placement{16, 12}, 1, Blend::Normal);
    dst.reset();
    ASSERT_TRUE(gpu->finish(next));
    expectNear(*next, expected);
}

TEST_F(GpuRendering, TransitionsMatchCpuAtCutsAndIntermediateTimes) {
    auto a = pattern(17, 11), b = pattern(17, 11);
    std::reverse(b->px.begin(), b->px.end());
    for (auto type : {"crossfade", "wipe", "dip", "slide"}) {
        for (auto direction : {"left", "right", "up", "down"}) {
            for (double mix : {0.0, 0.25, 0.5, 0.9, 1.0}) {
                SCOPED_TRACE(::testing::Message() << type << ' ' << direction << ' ' << mix);
                ParamFn params = [&](std::string_view name) -> Value {
                    if (name == "direction") return std::string(direction);
                    if (name == "softness") return 0.12;
                    if (name == "color") return Color{0.7f, 0.2f, 0.5f, 0.6f};
                    return {};
                };
                Image expected;
                mixTransition(expected, *a, *b, type, mix, params);
                auto dst = std::make_shared<Image>(17, 11);
                auto gpu = createGpuFrame();
                gpu->transition(dst, a, b, type, mix, params);
                ASSERT_TRUE(gpu->finish(dst));
                expectNear(*dst, expected);
            }
        }
    }
}

TEST_F(GpuRendering, SolidTransformsMatchRasterizedLayers) {
    const Color color{0.8f, 0.3f, 0.6f, 0.7f};
    auto src = std::make_shared<Image>(19, 7);
    fill(*src, color);
    for (const Placement& at : {Placement{8.5, 5.5}, {5.3, 9.2, 1.7, 0.8, 0.4}, {16, 12, 0.6, 1.4, 0}}) {
        auto dst = std::make_shared<Image>(32, 24);
        Image expected(32, 24);
        drawLayer(expected, *src, at, 0.8f, Blend::Screen);
        auto gpu = createGpuFrame();
        gpu->fill(dst, Color{0, 0, 0, 0});
        gpu->drawSolid(dst, src->w, src->h, color, at, 0.8f, Blend::Screen);
        ASSERT_TRUE(gpu->finish(dst));
        expectNear(*dst, expected);
    }
}

TEST_F(GpuRendering, EffectsMatchCpuAndKeepTheSourceImmutable) {
    ParamFn params = [](std::string_view name) -> Value {
        if (name == "brightness") return 0.13;
        if (name == "contrast") return 1.3;
        if (name == "amount") return 0.7;
        if (name == "mix") return 0.6;
        if (name == "softness") return 0.35;
        if (name == "radius") return 9.0;
        if (name == "color") return Color{0.7f, 0.3f, 0.8f, 0.6f};
        if (name == "level") return 0.43;
        if (name == "left") return 0.17;
        if (name == "right") return 0.13;
        if (name == "top") return 0.23;
        if (name == "bottom") return 0.11;
        if (name == "width") return 19.0;
        if (name == "height") return 11.0;
        if (name == "x") return 2.3;
        if (name == "y") return -1.7;
        if (name == "feather") return 3.2;
        if (name == "shape") return std::string("ellipse");
        if (name == "keep") return std::string("outside");
        return {};
    };
    for (auto [w, h] : {std::pair{1, 1}, {3, 5}, {31, 19}, {65, 33}}) {
        for (auto type :
             {"brightness_contrast", "saturation", "invert", "tint", "vignette", "threshold", "crop", "mask", "blur"}) {
            SCOPED_TRACE(::testing::Message() << type << ' ' << w << 'x' << h);
            auto source = pattern(w, h);
            Image original = *source, processed = original, expected(w, h);
            PixelBox content{0, 0, w, h};
            if (w > 10 && h > 10) content = {2, 3, w - 5, h - 7};
            applyEffect(processed, type, params, 0.7, content);
            drawLayer(expected, processed, Placement{w / 2.0, h / 2.0}, 1, Blend::Normal);
            auto dst = std::make_shared<Image>(w, h);
            auto gpu = createGpuFrame();
            gpu->fill(dst, Color{0, 0, 0, 0});
            gpu->applyEffects(source, {{type, params}}, 0.7, content);
            gpu->applyEffects(source, {{type, params}}, 0.7, content);
            gpu->drawLayer(dst, source, Placement{w / 2.0, h / 2.0}, 1, Blend::Normal);
            ASSERT_TRUE(gpu->finish(dst));
            expectNear(*dst, expected, 1);
            EXPECT_EQ(source->px, original.px);
        }
    }
}

TEST_F(GpuRendering, BlurHandlesRadiiLargerThanEitherDimension) {
    for (auto [w, h] : {std::pair{1, 1}, {1, 9}, {11, 1}, {3, 5}, {67, 33}}) {
        for (double radius : {0.0, 2.0, 6.0, 48.0}) {
            auto image = pattern(w, h);
            Image expected = *image;
            ParamFn params = [=](std::string_view name) -> Value { return name == "radius" ? Value{radius} : Value{}; };
            applyEffect(expected, "blur", params, 1);
            auto gpu = createGpuFrame();
            gpu->applyEffects(image, {{"blur", params}}, 1, {});
            ASSERT_TRUE(gpu->finish(image));
            expectNear(*image, expected);
        }
    }
}

TEST(Gpu, RendererSelectsCpuForSmallPreviewsAndCanForceMetal) {
    Project p = Project::makeDefault();
    p.settings.width = 64;
    p.settings.height = 32;
    p.script.clear();
    Clip c;
    c.kind = ClipKind::Solid;
    c.track = p.findTrackByName("V1")->id;
    c.duration = 2;
    Id id = edit::addClip(p, c);
    edit::setPropConstant(p, id, "color", Color{0.3f, 0.6f, 0.7f, 1});
    MediaCache cache;
    Evaluator ev;
    ev.setProject(p);
    FrameRenderer automatic(cache);
    auto expected = automatic.render(ev, 0, 64, 32);
    EXPECT_FALSE(automatic.usedGpuLastFrame());
    FrameRenderer metal(cache, RenderBackend::Metal);
    auto actual = metal.render(ev, 0, 64, 32);
    EXPECT_EQ(metal.usedGpuLastFrame(), gpuAvailable());
    expectNear(*actual, *expected);
}
