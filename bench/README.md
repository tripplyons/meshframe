# Render benchmark

`meshframe_bench` renders every project in `bench/projects` for 60 frames at
1920x1080 and reports how much work the renderer did. The metric is a set of
integer counters from `src/core/work_counter.hpp`: pixels blended, source
samples, blur accumulator steps, pixels copied, raster and glyph pixels, decoded
frames, and so on. Their sum is the score.

The counters are integers summed over every thread, so the same code reports the
same totals on every run, whatever the thread pool did. Wall clock moves with
CPU frequency, scheduling, and cache state, so the benchmark prints it to stderr
as a diagnostic and never scores it.

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
./build/meshframe_bench --frames 60 --expect bench/expected_digests.txt
./build/meshframe_bench --frames 60 --json    # the score on stdout
```