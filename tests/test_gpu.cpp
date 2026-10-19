#include "fixtures.hpp"
#include "engine/frame_renderer.hpp"
#include "model/edit.hpp"
#include "render/gpu.hpp"
#include "render/raster.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

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
    for (auto [w, h] : {std::pair{1, 1}, {1, 9}, {11, 1}, {3, 5}, {67, 33}, {129, 7}, {257, 33}, {513, 129}}) {
        for (double radius : {0.0, 2.0, 6.0, 8.0, 18.0, 48.0, 64.0, 66.0, 140.0, 256.0, 258.0}) {
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

TEST_F(GpuRendering, RendererKeepsLargeBlurAndCpuFallbackConsistent) {
    auto probe = createGpuFrame();
    EXPECT_TRUE(probe->supportsBlur(0));
    EXPECT_TRUE(probe->supportsBlur(64));
    EXPECT_FALSE(probe->supportsBlur(300));
    Project project = Project::makeDefault();
    project.settings.width = 64;
    project.settings.height = 32;
    project.script = R"(mf.generator("half", function(g) g:rect(0, 0, 32, 32, gray(1)) end))";
    Clip clip;
    clip.kind = ClipKind::Generator;
    clip.track = project.findTrackByName("V1")->id;
    clip.duration = 2;
    Id id = edit::addClip(project, clip);
    edit::setPropConstant(project, id, "generator", std::string("half"));
    Id effect = edit::addEffect(project, id, false, "blur");
    for (double radius : {64.0, 140.0, 300.0}) {
        SCOPED_TRACE(radius);
        edit::setPropConstant(project, effect, "radius", radius);
        MediaCache cache;
        FrameRenderer cpu(cache, RenderBackend::Cpu), metal(cache, RenderBackend::Metal);
        Evaluator evaluator;
        evaluator.setProject(project);
        auto expected = cpu.render(evaluator, 0, 64, 32);
        auto actual = metal.render(evaluator, 0, 64, 32);
        EXPECT_TRUE(metal.usedGpuLastFrame());
        expectNear(*actual, *expected);
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

TEST_F(GpuRendering, AlignedSolidsMatchRasterizedColorsAcrossClippingAndBlends) {
    const Color background{.2f, .4f, .6f, .7f};
    for (Color color : {Color{.8f, .3f, .6f, 0}, Color{.8f, .3f, .6f, 1.0f / 255},
                       Color{.8f, .3f, .6f, .7f}, Color{.8f, .3f, .6f, 1}}) {
        auto source = std::make_shared<Image>(19, 7);
        fill(*source, color);
        for (const Placement at : {Placement{8.5, 5.5}, Placement{-2.5, -1.5}, Placement{37.5, 28.5}})
            for (float opacity : {.4f, 1.0f})
                for (Blend mode : {Blend::Normal, Blend::Add, Blend::Multiply, Blend::Screen}) {
                    auto expected = std::make_shared<Image>(32, 24), actual = std::make_shared<Image>(32, 24);
                    auto reference = createGpuFrame();
                    reference->fill(expected, background);
                    reference->drawLayer(expected, source, at, opacity, mode);
                    ASSERT_TRUE(reference->finish(expected));
                    auto gpu = createGpuFrame();
                    gpu->fill(actual, background);
                    gpu->drawSolid(actual, source->w, source->h, color, at, opacity, mode);
                    ASSERT_TRUE(gpu->finish(actual));
                    EXPECT_EQ(actual->px, expected->px);
                }
    }
}

TEST_F(GpuRendering, GeneratorRectanglesMatchCpuCoverageAndCommandOrder) {
    for (float scale : {.5f, 1.0f, 2.0f})
        for (bool clear : {false, true}) {
            DrawList list;
            if (clear) list.cmds.push_back({DrawCmd::Type::Clear, 0, 0, 0, 0, 0, Color{.1f, .3f, .6f, .7f}});
            for (const auto& rect : {std::array<float, 4>{-3.7f, 2.125f, 13.4f, 8.6f},
                                     {13.75f, 17.25f, -9.5f, -7.125f}, {15.8f, -3.9f, .3f, 11.75f},
                                     {2, 2, 20, 13}, {7.125f, 8.375f, 6.625f, .25f}})
                for (Color color : {Color{.9f, .3f, .6f, .4f}, Color{.2f, .7f, .4f, 1}})
                    list.cmds.push_back({DrawCmd::Type::Rect, rect[0], rect[1], rect[2], rect[3], 0, color});
            auto actual = std::make_shared<Image>(37, 29);
            fill(*actual, Color{.2f, .4f, .6f, .7f});
            Image expected = *actual;
            drawList(expected, list, scale);
            auto gpu = createGpuFrame();
            ASSERT_TRUE(gpu->rasterize(actual, list, scale));
            ASSERT_TRUE(gpu->finish(actual));
            expectNear(*actual, expected, 2);
        }
}

TEST_F(GpuRendering, UnsupportedGeneratorListsEncodeNoPartialRaster) {
    auto actual = pattern(13, 9);
    const Image expected = *actual;
    DrawList list;
    list.cmds.push_back({DrawCmd::Type::Clear, 0, 0, 0, 0, 0, Color{1, 0, 0, 1}});
    list.cmds.push_back({DrawCmd::Type::Circle, 5, 5, 3, 0, 0, Color{0, 1, 0, 1}});
    auto gpu = createGpuFrame();
    EXPECT_FALSE(gpu->rasterize(actual, list, 1));
    ASSERT_TRUE(gpu->finish(actual));
    EXPECT_EQ(actual->px, expected.px);
}

TEST_F(GpuRendering, GeneratorRasterAndUnsupportedEffectsKeepCpuFallbackConsistent) {
    for (auto [w, h] : {std::pair{64, 32}, std::pair{320, 240}})
        for (const char* generator : {"rects", "circles"})
            for (const char* effect : {"none", "threshold", "drop_shadow"}) {
                SCOPED_TRACE(::testing::Message() << generator << ' ' << effect);
                Project project = Project::makeDefault();
                project.settings.width = w;
                project.settings.height = h;
                project.script = R"(
                    mf.generator("rects", function(g) g:rect(3.25, 4.75, g.w - 20.5, g.h - 14.875, hsv(0.7, 0.6, 0.9, 0.7)) end)
                    mf.generator("circles", function(g) g:clear(gray(0.2)); g:circle(28, 16, 12, hsv(0.4, 0.5, 0.8)) end)
                )";
                Clip clip;
                clip.kind = ClipKind::Generator;
                clip.track = project.findTrackByName("V1")->id;
                clip.duration = 2;
                Id id = edit::addClip(project, clip);
                edit::setPropConstant(project, id, "generator", std::string(generator));
                if (std::string_view(effect) != "none") edit::addEffect(project, id, false, effect);
                MediaCache cache;
                FrameRenderer cpu(cache, RenderBackend::Cpu), metal(cache, RenderBackend::Metal);
                Evaluator evaluator;
                evaluator.setProject(project);
                auto expected = cpu.render(evaluator, 0, w, h);
                auto actual = metal.render(evaluator, 0, w, h);
                ASSERT_TRUE(metal.usedGpuLastFrame());
                expectNear(*actual, *expected, 2);
            }
}

TEST_F(GpuRendering, EffectBatchesPreserveIntermediateRoundingAndSpatialBoundaries) {
    ParamFn params = [](std::string_view name) -> Value {
        if (name == "brightness") return .013;
        if (name == "contrast") return 1.03;
        if (name == "amount") return .83;
        if (name == "mix") return .17;
        if (name == "softness") return .7;
        if (name == "radius") return 9.0;
        if (name == "color") return Color{.7f, .3f, .8f, .6f};
        if (name == "left") return .013;
        if (name == "right") return .017;
        if (name == "top") return .023;
        if (name == "bottom") return .011;
        if (name == "width") return 19.0;
        if (name == "height") return 11.0;
        if (name == "x") return 2.3;
        if (name == "y") return -1.7;
        if (name == "feather") return 3.2;
        if (name == "shape") return std::string("ellipse");
        if (name == "keep") return std::string("outside");
        return {};
    };
    for (auto [w, h] : {std::pair{17, 9}, {67, 33}, {257, 257}})
        for (bool longChain : {false, true}) {
            std::vector<GpuEffect> effects;
            const std::array<const char*, 9> types{
                "brightness_contrast", "saturation", "invert", "tint", "vignette", "crop", "mask", "blur", "drop_shadow"};
            for (int i = 0; i < (longChain ? 70 : 22); ++i)
                effects.push_back({longChain ? types[i % 5] : types[i % types.size()], params});
            auto source = pattern(w, h), expected = std::make_shared<Image>(*source);
            Image original = *source;
            const PixelBox content{2, 1, w - 4, h - 3};
            // Finish each single effect through the public GPU interface. This
            // reference materializes exactly the byte boundary every old pass used.
            for (const auto& effect : effects) {
                auto gpu = createGpuFrame();
                gpu->applyEffects(expected, {effect}, .7, content);
                ASSERT_TRUE(gpu->finish(expected));
            }
            auto actual = std::make_shared<Image>(w, h);
            auto gpu = createGpuFrame();
            gpu->applyEffects(source, effects, .7, content);
            gpu->drawLayer(actual, source, Placement{w / 2.0, h / 2.0}, 1, Blend::Normal);
            ASSERT_TRUE(gpu->finish(actual));
            expectNear(*actual, *expected, 1);
            EXPECT_EQ(source->px, original.px);
        }
}

TEST_F(GpuRendering, DropShadowsMatchCpuForColorOffsetsAndBlurRadii) {
    auto probe = createGpuFrame();
    ASSERT_TRUE(probe->supportsDropShadow(9));
    EXPECT_FALSE(probe->supportsDropShadow(300));
    for (auto [w, h] : {std::pair{1, 1}, {3, 5}, {31, 19}, {257, 257}})
        for (double radius : {0.0, 2.0, 9.0, 66.0, 140.0})
            for (auto [x, y] : {std::pair{0.0, 0.0}, {2.3, -1.7}, {-400.0, 300.0}})
                for (Color color : {Color{0, 0, 0, 1}, Color{.7f, .3f, .8f, .6f}}) {
                    SCOPED_TRACE(::testing::Message() << w << 'x' << h << " blur " << radius << " offset " << x << ',' << y);
                    ParamFn params = [=](std::string_view name) -> Value {
                        if (name == "blur") return radius;
                        if (name == "x") return x;
                        if (name == "y") return y;
                        if (name == "color") return color;
                        if (name == "opacity") return .73;
                        return {};
                    };
                    auto source = pattern(w, h);
                    const Image original = *source;
                    Image expected = original;
                    applyEffect(expected, "drop_shadow", params, .7);
                    auto actual = std::make_shared<Image>(w, h);
                    auto gpu = createGpuFrame();
                    gpu->applyEffects(source, {{"drop_shadow", params}}, .7, {});
                    gpu->drawLayer(actual, source, Placement{w / 2.0, h / 2.0}, 1, Blend::Normal);
                    ASSERT_TRUE(gpu->finish(actual));
                    expectNear(*actual, expected, 1);
                    EXPECT_EQ(source->px, original.px);
                }
}

TEST_F(GpuRendering, CachedMediaShadowKeepsContentBoundsForFollowingEffects) {
    if (!fixtures::haveFfmpeg()) GTEST_SKIP() << "ffmpeg CLI not installed";
    Project project = Project::makeDefault();
    project.script.clear();
    project.settings.width = 64;
    project.settings.height = 32;
    MediaSource source;
    std::string error;
    ASSERT_TRUE(probeMedia(fixtures::redImage(), source, &error)) << error;
    Id media = edit::addMedia(project, source);
    Clip clip;
    clip.kind = ClipKind::Media;
    clip.media = media;
    clip.track = project.findTrackByName("V1")->id;
    clip.duration = 2;
    Id id = edit::addClip(project, clip);
    Id shadow = edit::addEffect(project, id, false, "drop_shadow");
    edit::setPropConstant(project, shadow, "blur", 9.0);
    edit::setPropConstant(project, shadow, "x", -6.0);
    edit::setPropConstant(project, shadow, "y", 4.0);
    Id vignette = edit::addEffect(project, id, false, "vignette");
    edit::setPropConstant(project, vignette, "amount", .7);
    edit::setPropConstant(project, vignette, "softness", .8);
    Id crop = edit::addEffect(project, id, false, "crop");
    edit::setPropConstant(project, crop, "left", .17);
    edit::setPropConstant(project, crop, "right", .13);
    edit::setPropConstant(project, crop, "top", .2);
    MediaCache cache;
    FrameRenderer cpu(cache, RenderBackend::Cpu), metal(cache, RenderBackend::Metal);
    Evaluator evaluator;
    evaluator.setProject(project);
    for (auto [w, h] : {std::pair{64, 32}, {96, 48}})
        for (int repeat = 0; repeat < 2; ++repeat) {
            auto expected = cpu.render(evaluator, 0, w, h);
            auto actual = metal.render(evaluator, 0, w, h);
            ASSERT_TRUE(metal.usedGpuLastFrame());
            expectNear(*actual, *expected, 2);
        }
}

TEST_F(GpuRendering, RecycledBuffersKeepConcurrentAndAbandonedFramesIndependent) {
    std::array<std::thread, 4> workers;
    for (size_t worker = 0; worker < workers.size(); ++worker)
        workers[worker] = std::thread([worker] {
            ParamFn params = [worker](std::string_view name) -> Value {
                if (name == "radius") return 9.0;
                if (name == "amount") return .2 + worker * .1;
                if (name == "color") return Color{.7f, .3f, .8f, .6f};
                return {};
            };
            for (int repeat = 0; repeat < 8; ++repeat) {
                int w = repeat % 2 ? 257 : 67, h = repeat % 2 ? 257 : 33;
                auto source = pattern(w, h);
                const Image original = *source;
                Image expected = original;
                applyEffect(expected, "blur", params, .7);
                applyEffect(expected, "tint", params, .7);
                // Discard encoded work without submitting it. These leases
                // must become reusable without allowing abandoned writes.
                {
                    auto abandoned = createGpuFrame();
                    abandoned->applyEffects(source, {{"blur", params}, {"tint", params}}, .7, {});
                }
                auto actual = std::make_shared<Image>(w, h);
                auto gpu = createGpuFrame();
                gpu->applyEffects(source, {{"blur", params}, {"tint", params}}, .7, {});
                gpu->drawLayer(actual, source, Placement{w / 2.0, h / 2.0}, 1, Blend::Normal);
                ASSERT_TRUE(gpu->finish(actual));
                expectNear(*actual, expected, 1);
                EXPECT_EQ(source->px, original.px);
            }
        });
    for (auto& worker : workers) worker.join();
}

TEST_F(GpuRendering, EmptyFramesOverwriteRecycledStorageAtOddDimensions) {
    Project project = Project::makeDefault();
    project.clips.clear();
    project.script.clear();
    MediaCache cache;
    FrameRenderer cpu(cache, RenderBackend::Cpu), metal(cache, RenderBackend::Metal);
    Evaluator evaluator;
    for (auto [w, h] : {std::pair{37, 29}, {65, 33}, {327, 241}})
        for (Color background : {Color{.9f, .1f, .3f, 1}, Color{.2f, .4f, .7f, .3f}, Color{0, 0, 0, 0}}) {
            project.settings.width = w;
            project.settings.height = h;
            project.settings.background = background;
            evaluator.setProject(project);
            auto expected = cpu.render(evaluator, 0, w, h);
            auto actual = metal.render(evaluator, 0, w, h);
            ASSERT_TRUE(metal.usedGpuLastFrame());
            EXPECT_EQ(actual->px, expected->px);
        }
}
