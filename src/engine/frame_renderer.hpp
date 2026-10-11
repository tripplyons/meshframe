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

private:
    struct Layer {
        std::shared_ptr<Image> image;
        double displayW = 0, displayH = 0;  // size in output pixels before the clip's scale
    };
    Layer content(Evaluator& ev, const Clip& c, double t, double unit, int w, int h);
    // Draws a clip with its effects and transform; returns false when it has no content.
    bool drawClip(Evaluator& ev, Image& dst, const Clip& c, double t, double unit, bool useBlend);

    MediaCache& cache_;
};

}  // namespace mf
