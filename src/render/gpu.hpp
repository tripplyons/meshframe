#pragma once

#include "render/compositor.hpp"

#include <memory>
#include <string>

namespace mf {

struct DrawList;

enum class RenderBackend { Auto, Cpu, Metal };

struct GpuEffect {
    std::string type;
    ParamFn params;
};

// A frame's compositing commands share GPU buffers. finish() synchronizes the
// pixels, so transitions and stacked layers do not make intermediate round trips.
class GpuFrame {
  public:
    virtual ~GpuFrame() = default;
    // Radius is measured in source-image pixels. Unsupported chains stay CPU-side.
    virtual bool supportsBlur(double radius) const = 0;
    virtual bool supportsDropShadow(double blurRadius) const = 0;
    virtual void fill(const std::shared_ptr<Image>& dst, Color color) = 0;
    // Draws supported generator commands into dst. Unsupported lists return
    // false without encoding anything, so the caller can rasterize on the CPU.
    virtual bool rasterize(const std::shared_ptr<Image>& dst, const DrawList& list, float scale) = 0;
    virtual void drawLayer(const std::shared_ptr<Image>& dst, ImagePtr src, const Placement& at, float opacity,
                           Blend blend) = 0;
    virtual void drawSolid(const std::shared_ptr<Image>& dst, int w, int h, Color color, const Placement& at,
                           float opacity, Blend blend) = 0;
    virtual void transition(const std::shared_ptr<Image>& dst, ImagePtr a, ImagePtr b, std::string_view type,
                            double mix, const ParamFn& params) = 0;
    // A layer's immutable source identifies its chain for this frame. Repeated
    // calls reuse the processed GPU result without changing the CPU pixels.
    virtual void applyEffects(ImagePtr image, const std::vector<GpuEffect>& effects, double unit, PixelBox content) = 0;
    // Synchronizes shared pixels or downloads the final canvas. On failure,
    // discard dst and retry on the CPU because GPU writes may be partial.
    virtual bool finish(const std::shared_ptr<Image>& dst) = 0;
    virtual double elapsedSeconds() const = 0;
};

std::unique_ptr<GpuFrame> createGpuFrame();
bool gpuAvailable();
std::string gpuError();

} // namespace mf
