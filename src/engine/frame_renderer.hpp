#pragma once

#include "engine/evaluator.hpp"
#include "media/media_cache.hpp"
#include "render/image.hpp"

namespace mf {

// Renders a project frame: evaluates every visible video clip at time t,
// applies its effects and transitions, and composites tracks bottom to top.
class FrameRenderer {
public:
    explicit FrameRenderer(MediaCache& cache) : cache_(cache) {}

    // Output is w x h; smaller than the project size gives a scaled preview.
    // Must run on the evaluator's thread.
    std::shared_ptr<Image> render(Evaluator& ev, double t, int w, int h);
    // Renders one video clip alone over transparency at project size, with
    // `pad` pixels of margin on each side so content beyond the frame shows.
    std::shared_ptr<Image> renderClip(Evaluator& ev, const Clip& c, double t, int padX, int padY);

private:
    struct Layer {
        std::shared_ptr<Image> image;
        double displayW = 0, displayH = 0;  // size in output pixels before the clip's scale
    };
    Layer content(Evaluator& ev, const Clip& c, double t, double unit, int w, int h);
    // Draws a clip with its effects and transform; returns false when it has no
    // content. The frame is dst less `pad` pixels on each side.
    bool drawClip(Evaluator& ev, Image& dst, const Clip& c, double t, double unit, bool useBlend, int padX = 0, int padY = 0);

    MediaCache& cache_;
};

}  // namespace mf
