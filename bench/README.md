# Render benchmark

`meshframe_bench` renders every project in `bench/projects` for 60 frames at
1920x1080 and reports how much work the renderer did. The metric is a set of
integer counters from `src/core/work_counter.hpp`: pixels blended, source
samples, blur accumulator steps, pixels copied, raster and glyph pixels, decoded
frames, and so on. Their sum is the score.

The counters are integers summed over every thread and GPU threadgroup, so the
same code and backend report the same totals on every run. The CPU campaign
scripts score this deterministic work. GPU throughput differs from CPU
throughput, so compare backend speed with repeated rendering times instead.

Correctness is a digest: `--expect bench/expected_digests.txt` hashes every
rendered frame and fails unless the frames are bit-identical to the ones at that
commit. The counter definitions themselves must not change, which the campaign's
scripts check with `git diff`.

## Contents

- `make-media.sh` generates three test videos in `bench/media` with ffmpeg:
  1280x720 detail-heavy motion, 1920x1080 smooth gradients, and 640x360 color
  bars with a frame counter. The files are ignored by git, because encoder
  output is not reproducible byte for byte.
- `projects/make-projects.sh` builds the projects through `meshframe-cli`.
  - `layers.mfproj`: stacked video layers with cover-fit scaling, blend modes,
    per-pixel effects (saturation, blur, vignette, ellipse mask, brightness),
    and a wipe and a dip transition.
  - `text_paths.mfproj`: outlined text, a stroke reveal with a shape morph, a
    path animated along a motion path, and drop shadows over a blurred video.
  - `generators.mfproj`: a Lua generator and expressions rasterized every frame,
    with effects over solid, generator, text, and path clips.
- `expected_digests.txt`: the frame hashes for the code at this commit.

## Running it

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMESHFRAME_BENCH=ON
cmake --build build
bench/make-media.sh                 # once, needs ffmpeg
bench/projects/make-projects.sh     # only when the projects need to change
./build/meshframe_bench --backend cpu --frames 60 --expect bench/expected_digests.txt
./build/meshframe_bench --backend cpu --frames 60 --json  # deterministic CPU work
./build/meshframe_bench --backend metal --frames 60 --json
./build/meshframe_bench --frames 60 --compare-cpu --json
```

The automatic backend uses Metal on macOS for frames of at least 262,144
pixels and keeps smaller previews on the CPU. Metal composites a whole frame
in one command buffer, including transition surfaces. Media, generator, text, and
path layers use GPU blur, drop shadows, color effects, vignette, crop, and mask.
Completed text/path effect results remain cached between frames. Large blur
radii up to 512 source pixels use wider prefix tiles when the device supports
them; radii above 256 require 1,024-thread tiles. Unsupported radii and
threshold cutoffs keep their CPU paths.
Consecutive color, crop, and mask effects share a Metal dispatch while retaining
the byte rounding between effects. Blur and shadow passes separate these batches.
Generator lists containing clears and rectangles use Metal rasterization when
their clipped work averages at least 65,536 pixels per command. Smaller lists,
other drawing commands and CPU-only effect chains keep CPU rasterization.
GPU-generated content also stays on Metal when a shadow needs padded storage.
Optional rectangle and shadow shaders initialize only when first needed.
Apple silicon shares image memory with Metal; other devices download the final
canvas. A 64 MiB pool reuses page-backed image storage and lazily attached Metal
views between frames. Other platforms, unavailable devices, and failed GPU
commands fall back to the CPU. Configure `-DMESHFRAME_METAL=OFF` to build without Metal.
A separate 64 MiB pool reuses Metal scratch and statistics buffers after their
frame commands complete. Unsubmitted commands are discarded before reuse.

`--backend metal` requires actual GPU rendering and fails if any frame falls
back. `--compare-cpu` renders a CPU reference for every frame and reports the
maximum and mean absolute RGBA channel error. It fails above three channel
levels. This permits rounding differences from parallel floating-point math.
The pinned digests remain the exact CPU reference.

Use `render_seconds` to compare rendering speed; it excludes frame hashing and
CPU-reference comparisons. Each JSON result includes the backend and number of
GPU frames. `gpu_seconds` reports hardware execution time, which helps separate
kernel cost from CPU preparation and synchronization. The integer work counts
describe operations on each backend, not
their relative speed: parallel GPU sampling may perform more taps than the
CPU's sample caches while finishing sooner. Do timing comparisons without
`--compare-cpu`, and alternate backend order across repeated runs.

## Targeted feature benchmarks

`MESHFRAME_BENCH=ON` also builds `meshframe_features_bench`. It accepts a mode
and an iteration count and prints JSON with elapsed seconds and an output hash:

```sh
./build/meshframe_features_bench layout 500
./build/meshframe_features_bench outline 20
./build/meshframe_features_bench generator 100
./build/meshframe_features_bench resize-cache 16
./build/meshframe_features_bench peaks-linear 100
./build/meshframe_features_bench peaks 100
./build/meshframe_features_bench scopes 1000
./build/meshframe_features_bench threshold 200
./build/meshframe_features_bench sheet 200
```

`text`, `waveform`, and `decode` are also available. `decode` accepts an optional
media path after the iteration count. Media modes otherwise use the generated
assets in `bench/media`.

Setup and hashing are outside the measured interval. `peaks-linear` preserves
the original timeline range scan as a comparison for exact multiresolution
peak queries. `resize-cache` requests eight different limits that produce the
same decoded frame size. `sheet` composes twelve translucent thumbnail frames.

Measure export through `meshframe-cli export ... wait=true` with a fixed
project, range, dimensions, and CRF. Export overlaps encoding with rendering
using one queued frame; evaluation and media access stay on their original
thread. The x264 preset and CRF are unchanged. Compare decoded video and audio
as well as elapsed time, and test cancellation while encoding is active.

Compare medians from alternating before/after runs. Feature results describe
their specific workloads and should not be multiplied together or treated as
whole-project speedups.

## Editing and media workflow benchmarks

`meshframe_workflows_bench MODE ITERATIONS [VIDEO]` measures work outside the
pixel kernels. Run from the repository root. Setup is outside the timer; output
contains the mode, elapsed milliseconds, and a checksum.

```sh
./build-ar/meshframe_workflows_bench edit 300
./build-ar/meshframe_workflows_bench evaluate 1000
./build-ar/meshframe_workflows_bench synth 1000
./build-ar/meshframe_workflows_bench lookup 50
./build-ar/meshframe_workflows_bench diff 10
./build-ar/meshframe_workflows_bench preview 30
./build-ar/meshframe_workflows_bench preview-reuse 30
./build-ar/meshframe_workflows_bench recovery 1
./build-ar/meshframe_workflows_bench scenes 20
./build-ar/meshframe_workflows_bench decode-seek 90
```

Editing, evaluator, and diff modes use 2,000 simple clips. `lookup` also creates
1,999 transitions and queries every clip's visibility. `synth` renders blocks of
4,096 stereo samples. `preview` renders a 960x540 preview plus a separate
320x180 analysis frame; `preview-reuse` downsamples the current preview instead.
Reuse requires a preview at the same time and revision, at least as large as the
analysis frame. If scopes draw first, or there is no matching preview, they
render an analysis frame normally.

`recovery` measures scheduling latency on the calling thread, excluding the
background write. The worker retains an immutable project snapshot, and only
one write can run at a time. Saving or closing waits for an outstanding write
before removing the recovery files. This is a responsiveness improvement, not
a claim that disk writes themselves are faster.

`decode` reads consecutive frames; `decode-seek` requests a repeatable scrambled
sequence. Both accept an optional video path. To measure sparse reads through
a long GOP, generate a fixture and pass it to `decode-long`:

```sh
ffmpeg -nostdin -v error -y -f lavfi \
  -i testsrc2=size=1920x1080:rate=30:duration=12 \
  -c:v libx264 -preset veryfast -crf 23 -g 360 -keyint_min 360 \
  -sc_threshold 0 build-ar/long-gop.mp4
./build-ar/meshframe_workflows_bench decode-long 15 build-ar/long-gop.mp4
```

The decoder consults known keyframes before a long forward seek. It continues
forward when seeking would replay the current GOP. Backward reads and files
without a usable index retain the previous seek behavior.

On the M3 Max, five alternating before/after passes measured these medians:

| Workload | Before, ms | After, ms | Speedup |
| --- | ---: | ---: | ---: |
| 300 edits | 14.263 | 7.566 | 1.89x |
| 1,000 evaluator rebuilds | 197.742 | 190.128 | 1.04x |
| 1,000 synth blocks | 408.426 | 236.305 | 1.73x |
| 50 complete visibility scans | 249.772 | 0.400 | 624x |
| 10 project diffs | 812.678 | 72.802 | 11.16x |
| 30 preview and scopes frames | 267.354 | 128.730 | 2.08x |
| Recovery scheduling | 4.468 | 0.022 | 205x |
| 20 scene analyses | 840.057 | 834.920 | 1.006x |
| 15 sparse long-GOP reads | 646.044 | 232.281 | 2.78x |
| 90 scrambled reads | 1045.570 | 1049.560 | 1.00x |

These are targeted workloads, not whole-editor multipliers. VideoToolbox decode
with CPU frame download was also tested and rejected: the 90-frame workload
was about ten times slower than software decode on this machine. Four-thread
software decoding also regressed; automatic FFmpeg thread selection remains.
The export encoder, x264 preset, and CRF remain unchanged.
