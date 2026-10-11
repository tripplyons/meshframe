#!/usr/bin/env bash
# Builds a short demo project through meshframe-cli, renders a frame, and
# exports it. Run from the repository root after building:
#
#   examples/demo.sh [OUT_DIR]
#
# Writes OUT_DIR/demo.mfproj, OUT_DIR/demo.png, and OUT_DIR/demo.mp4
# (OUT_DIR defaults to ./demo-out).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cli="${MESHFRAME_CLI:-$root/build/meshframe-cli}"
out="${1:-demo-out}"
mkdir -p "$out"
project="$out/demo.mfproj"
rm -f "$project"

# One process runs every request, so they share one session and one history.
# Lines are split like shell words. Each reply is one JSON line; stop at the
# first failed command or reported error.
"$cli" --project "$project" - <<EOF | while IFS= read -r reply; do
settings width=1280 height=720 fps=30

# The script declares a signal, a generator, and a synth. Clips use them by name.
script.set "file=$root/examples/demo.lua"

# Background: a solid whose color drifts with time.
clip.add solid V1 0 8 bg
prop.expr bg color "hsv(0.6 + t * 0.02, 0.5, 0.18)"

# Spectrum-like bars driven by the beat signal, faded in by a keyframe.
clip.add generator V2 0 8 bars 'props={"generator":"bars","opacity":0.6}'
prop.key bars opacity 0 0
prop.key bars opacity 1.5 0.6 ease-out

# Title: fades in, bobs with an expression, and grows on each beat.
clip.add text V3 0.5 4 title 'props={"text":"meshframe","size":120}'
prop.key title opacity 0 0
prop.key title opacity 0.8 1 ease-out
prop.expr title y "-60 + sin(t * 2) * 12"
prop.expr title scale "1 + sig.pulse * 0.05"

# Second title, joined to the first with a wipe.
clip.add text V3 4.5 3.5 subtitle 'props={"text":"reactive video editing","size":64}'
transition.add title subtitle wipe 1
effect.add subtitle vignette

# Audio: the synth on A1 and a master compressor.
clip.add synth A1 0 8 tone 'props={"synth":"tone","gain":-14}'
effect.add master compressor

frame "$out/demo.png" 2
EOF
  if [[ "$reply" != *'"ok":true'* || "$reply" == *'"errors":[{'* ]]; then
    echo "demo: $reply" >&2
    exit 1
  fi
done

"$cli" --project "$project" export "$out/demo.mp4" wait=true >/dev/null
echo "wrote $project, $out/demo.png, and $out/demo.mp4"
