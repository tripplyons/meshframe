#include "render/gpu.hpp"

#ifndef MF_METAL
namespace mf {
std::unique_ptr<GpuFrame> createGpuFrame() { return nullptr; }
bool gpuAvailable() { return false; }
std::string gpuError() { return "Metal rendering is not enabled in this build"; }
} // namespace mf
#endif
