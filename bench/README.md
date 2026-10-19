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
in one command buffer, including transition surfaces. Media and generator
layers also use GPU blur, color effects, vignette, crop, and mask. Large blur
radii, threshold cutoffs, and cached text/path effects keep their CPU paths.
Apple silicon shares image memory with Metal; other devices download the final
canvas. A 64 MiB pool reuses page-backed image storage between frames. Other
platforms, unavailable devices, and failed GPU commands fall
back to the CPU. Configure `-DMESHFRAME_METAL=OFF` to build without Metal.

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
