#pragma once

#include "engine/evaluator.hpp"
#include "media/media_cache.hpp"
#include "render/image.hpp"
#include "render/gpu.hpp"

namespace mf {

// Renders a project frame: evaluates every visible video clip at time t,
// applies its effects and transitions, and composites tracks bottom to top.
class FrameRenderer {
public:
    explicit FrameRenderer(MediaCache& cache, RenderBackend backend = RenderBackend::Auto) : cache_(cache), backend_(backend) {}
    bool usedGpuLastFrame() const { return usedGpu_; }
    double gpuSecondsLastFrame() const { return gpuSeconds_; }

    // Output is w x h; smaller than the project size gives a scaled preview.
    // Must run on the evaluator's thread.
    std::shared_ptr<Image> render(Evaluator& ev, double t, int w, int h);
    // Renders one video clip alone over transparency at project size times
    // `scale`, with `pad` pixels of margin on each side so content beyond the
    // frame shows.
    std::shared_ptr<Image> renderClip(Evaluator& ev, const Clip& c, double t, double scale, int padX, int padY);

private:
    struct Layer {
        std::shared_ptr<Image> image;
        double offsetX = 0, offsetY = 0;  // layer center relative to the object origin, in output pixels
        double displayW = 0, displayH = 0;  // size in output pixels before the clip's scale
        bool opaque = false, solid = false, gpuPixels = false;
        Color color{};
        PixelBox effectContent{};
    };
    Layer content(Evaluator& ev, const Clip& c, double t, double unit, int w, int h, bool inspectOpacity,
                  GpuFrame* rasterGpu);
    // Draws a clip with its effects and transform; returns false when it has no
    // content. The frame is dst less `pad` pixels on each side.
    bool drawClip(Evaluator& ev, const std::shared_ptr<Image>& target, const Clip& c, double t, double unit,
                  bool useBlend, GpuFrame* gpu = nullptr, int padX = 0, int padY = 0);
    std::shared_ptr<Image> renderFrame(Evaluator& ev, double t, int w, int h, GpuFrame* gpu);

    MediaCache& cache_;
    RenderBackend backend_;
    bool usedGpu_ = false, fillBackground_ = false;
    double gpuSeconds_ = 0;
    Color background_{};
};

}  // namespace mf
