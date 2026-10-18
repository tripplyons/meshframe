#pragma once

#include <cstdint>
#include <string>

namespace mf {

// Deterministic counts of the work the renderer does in its inner loops. The
// counts are integers summed over every thread, so two runs of the same code
// on the same frames report the same totals no matter how the thread pool
// splits the work. Wall clock is not usable for comparison: it moves with CPU
// frequency, scheduling, and cache state. These totals are a proxy for the
// work itself, not a cycle count.
struct WorkCounts {
    uint64_t blendPixels = 0;    // pixels blended into a destination
    uint64_t sampleTaps = 0;     // source samples, such as bilinear taps or supersamples
    uint64_t mapPixels = 0;      // pixels run through a per-pixel effect callback
    uint64_t maskPixels = 0;     // pixels run through a mask, overlap, or alpha scale
    uint64_t blurSteps = 0;      // box blur accumulator steps
    uint64_t copyPixels = 0;     // pixels copied, filled, or interpolated
    uint64_t rasterPixels = 0;   // pixels covered by polygon or path fills
    uint64_t edgeTests = 0;      // edge crossings tested while rasterizing
    uint64_t glyphPixels = 0;    // glyph bitmap pixels drawn or rasterized
    uint64_t distancePixels = 0; // pixels visited by the outline distance transform
    uint64_t decodeFrames = 0;   // video frames pulled out of a decoder
    uint64_t scaledPixels = 0;   // pixels written by decode conversion and scaling
    uint64_t layerBuilds = 0;    // clip layers rasterized
    uint64_t evals = 0;          // reactive property evaluations
    uint64_t chunks = 0;         // parallel chunks dispatched

    uint64_t total() const;
    // One line per counter, then "total <n>". For logs.
    std::string toString() const;
    // All counters as one JSON object, including "total".
    std::string toJson() const;
};

// Counters for the calling thread. Every thread that touches the renderer gets
// its own set, and workCounts() adds them up.
WorkCounts& threadWorkCounts();

// Totals across every thread that has counted since the last reset.
WorkCounts workCounts();

// Zeroes every thread's counters.
void resetWorkCounts();

namespace work {

inline void addBlend(uint64_t n) { threadWorkCounts().blendPixels += n; }
inline void addTaps(uint64_t n) { threadWorkCounts().sampleTaps += n; }
inline void addMap(uint64_t n) { threadWorkCounts().mapPixels += n; }
inline void addMask(uint64_t n) { threadWorkCounts().maskPixels += n; }
inline void addBlur(uint64_t n) { threadWorkCounts().blurSteps += n; }
inline void addCopy(uint64_t n) { threadWorkCounts().copyPixels += n; }
inline void addRaster(uint64_t n) { threadWorkCounts().rasterPixels += n; }
inline void addEdge(uint64_t n) { threadWorkCounts().edgeTests += n; }
inline void addGlyph(uint64_t n) { threadWorkCounts().glyphPixels += n; }
inline void addDistance(uint64_t n) { threadWorkCounts().distancePixels += n; }
inline void addDecode(uint64_t n) { threadWorkCounts().decodeFrames += n; }
inline void addScaled(uint64_t n) { threadWorkCounts().scaledPixels += n; }
inline void addLayer(uint64_t n) { threadWorkCounts().layerBuilds += n; }
inline void addEval(uint64_t n) { threadWorkCounts().evals += n; }
inline void addChunk(uint64_t n) { threadWorkCounts().chunks += n; }

}  // namespace work
}  // namespace mf