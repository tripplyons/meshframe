#!/usr/bin/env bash
# Generates the test videos the render benchmark composites. Run from the
# repository root after building:
#
#   bench/make-media.sh
#
# Writes bench/media/clip_*.mp4. The files are ignored by git because ffmpeg
# output is not reproducible byte for byte; the benchmark generates them the
# same way in a fresh checkout. Deterministic inputs, so the rendered frames
# are identical from run to run.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/bench/media"
mkdir -p "$out"

if [[ -f "$out/clip_a.mp4" && -f "$out/clip_b.mp4" && -f "$out/clip_c.mp4" ]]; then
  exit 0
fi

common=(-hide_banner -loglevel error -y -an)

# Busy high-detail motion, the case bilinear scaling and compositing cost most.
ffmpeg "${common[@]}" -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=3" \
  -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -g 15 "$out/clip_a.mp4"

# Smooth gradients at frame size, which is the case bilinear scaling costs most.
ffmpeg "${common[@]}" -f lavfi -i "gradients=size=1920x1080:rate=30:duration=3:speed=0.05" \
  -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -g 15 "$out/clip_b.mp4"

# Color bars with a frame counter drawn in, cheap to decode and easy to compare.
ffmpeg "${common[@]}" -f lavfi -i "testsrc=size=640x360:rate=30:duration=3:decimals=2" \
  -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p -g 15 "$out/clip_c.mp4"