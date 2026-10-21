#!/usr/bin/env bash
# Generates the test videos the render benchmark composites. Run from the
# repository root after building:
#
#   bench/make-media.sh
#
# Writes bench/media/clip_*.mp4. The files are ignored by git because ffmpeg
# output is not reproducible byte for byte; the benchmark generates them the
# same way in a fresh checkout. Each clip is lossless and needs no color
# conversion, so the decoded frames, and with them the pinned digests, do not
# change between x264 and ffmpeg versions.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/bench/media"
mkdir -p "$out"

# Regenerate when this script changes, so old clips never meet new digests.
recipe="$(shasum -a 256 "$0" | cut -d' ' -f1)"
if [[ -f "$out/clip_a.mp4" && -f "$out/clip_b.mp4" && -f "$out/clip_c.mp4" &&
      "$(cat "$out/recipe" 2>/dev/null)" == "$recipe" ]]; then
  exit 0
fi

common=(-hide_banner -loglevel error -y -an)

# Busy high-detail motion, the case bilinear scaling and compositing cost most.
ffmpeg "${common[@]}" -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=3" \
  -c:v libx264 -preset veryfast -qp 0 -pix_fmt yuv420p -g 15 "$out/clip_a.mp4"

# Full frame size with a moving alpha pattern, the case bilinear scaling costs most.
# Sources must be byte-identical every run, so nothing random goes in them.
ffmpeg "${common[@]}" -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=3:alpha=200" \
  -c:v libx264 -preset veryfast -qp 0 -pix_fmt yuv420p -g 15 "$out/clip_b.mp4"

# Color bars with a frame counter drawn in, cheap to decode and easy to compare.
# testsrc draws only RGB, so it is encoded as RGB to skip swscale.
ffmpeg "${common[@]}" -f lavfi -i "testsrc=size=640x360:rate=30:duration=3:decimals=2" \
  -c:v libx264rgb -preset veryfast -qp 0 -pix_fmt rgb24 -g 15 "$out/clip_c.mp4"

echo "$recipe" > "$out/recipe"