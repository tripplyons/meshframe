#!/usr/bin/env bash
# Builds the benchmark projects through meshframe-cli. Run from the repository
# root after building and after bench/make-media.sh:
#
#   bench/projects/make-projects.sh
#
# Writes bench/projects/*.mfproj. The projects are committed; this script is
# how they are regenerated. They cover the expensive parts of a frame: decode
# and scaling, layered compositing with blend modes, per-pixel effects,
# transitions, text, and vector paths.
#
# Media is imported by a path relative to the repository root, because that is
# how the importer resolves paths. The saved projects keep the paths relative
# to the project file, so they load from anywhere.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cli="${MESHFRAME_CLI:-$root/build/meshframe-cli}"
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$here"

# One process runs every request, so they share one session and one history.
run() {
  local project="$1"
  rm -f "$project"
  "$cli" --project "$project" - | while IFS= read -r reply; do
    if [[ "$reply" != *'"ok":true'* || "$reply" == *'"errors":[{'* ]]; then
      echo "bench project $(basename "$project"): $reply" >&2
      exit 1
    fi
  done
}

# Stacked video layers: cover-fit scaling, blend modes, per-pixel effects, and
# two transition types over the same source clips.
run "$here/layers.mfproj" <<EOF
settings width=1920 height=1080 fps=30
media.import bench/media/clip_a.mp4
media.import bench/media/clip_b.mp4
media.import bench/media/clip_c.mp4

clip.media clip_a.mp4 0 V1 A1
clip.media clip_b.mp4 0 V2
clip.media clip_c.mp4 0 V3

prop.set clip_a.mp4 fit cover
prop.set clip_b.mp4 fit cover
prop.set clip_c.mp4 fit cover
prop.set clip_c.mp4 scale 1.4
prop.set clip_c.mp4 rotation 6
prop.set clip_b.mp4 opacity 0.7
prop.set clip_b.mp4 blend screen
prop.expr clip_c.mp4 x "60 * sin(t * 1.4)"
prop.expr clip_c.mp4 y "40 * cos(t * 0.9)"

effect.add clip_a.mp4 saturation
effect.add clip_b.mp4 blur
effect.add clip_b.mp4 vignette
effect.add clip_c.mp4 mask
effect.add clip_c.mp4 brightness_contrast

EOF

# Text with an outline, vector paths with strokes and shadows, a stroke reveal
# with a morph, and a path animated along a motion path.
run "$here/text_paths.mfproj" <<EOF
settings width=1920 height=1080 fps=30
media.import bench/media/clip_b.mp4
media.import bench/media/clip_a.mp4

clip.media clip_b.mp4 0 V1
prop.set clip_b.mp4 fit cover
effect.add clip_b.mp4 blur

clip.add text V2 0 3 title 'props={"text":"meshframe renders on the CPU","size":132,"wrap":1500,"align":"center","outline":10}'
effect.add title drop_shadow

clip.add text V3 0 3 sub 'props={"text":"every pixel counted","size":72,"align":"right"}'
prop.expr sub y "380 + 20 * sin(t * 2)"
effect.add sub tint

clip.add path V2 0.2 2.8 star 'props={"path":"M 0 -260 L 70 -80 L 250 -80 L 110 40 L 160 240 L 0 130 L -160 240 L -110 40 L -250 -80 L -70 -80 Z","width":520,"height":520,"fill":"#f5c542","stroke":"#1b1b1b","strokeWidth":14,"strokeLinejoin":"miter","strokeLinecap":"round"}'
effect.add star drop_shadow

clip.add path V3 0.4 2.6 morph 'props={"width":700,"height":420,"fill":"#3b7dd8","stroke":"#0b1b2a","strokeWidth":10}'
prop.expr morph trimEnd "fmod(t * 0.35, 1)"
prop.expr morph morph "0.5 + 0.5 * sin(t * 1.1)"

clip.add path V3 0.6 2.4 orbit 'props={"path":"M -260 0 C -120 -180 120 180 260 0 Z","width":420,"height":260,"fill":"#00000000","stroke":"#e0503a","strokeWidth":18,"strokeLinecap":"round"}'
prop.set orbit motionPath "M -700 -300 C -200 -420 240 380 700 260"
prop.expr orbit motionProgress "fmod(t * 0.2, 1)"
prop.set orbit motionRotate 1

clip.add path V2 1 2 ellipse 'props={"shape":"ellipse","width":300,"height":300,"fill":"#ffffff","stroke":"#000000","strokeWidth":8,"fillRule":"evenodd"}'
effect.add ellipse drop_shadow

transition.add title star crossfade 0.4
EOF

# Generators and expressions: Lua rasterized every frame, plus effects over
# solid, generator, and text clips.
run "$here/generators.mfproj" <<EOF
settings width=1920 height=1080 fps=30
script.set "file=$root/examples/demo.lua"

clip.add solid V1 0 3 bg
prop.expr bg color "hsv(0.6 + t * 0.02, 0.5, 0.16)"

track.add video V4

clip.add generator V2 0 3 bars 'props={"generator":"bars","opacity":0.75}'
effect.add bars brightness_contrast
effect.add bars vignette

clip.add generator V3 0.3 2.7 rings 'props={"generator":"bars","opacity":0.5}'
prop.expr rings scale "1.1 + 0.15 * sin(t * 2.4)"
prop.expr rings rotation "8 * t"
effect.add rings invert

clip.add text V4 0.2 2.6 label 'props={"text":"counted, not timed","size":96,"align":"center","outline":6}'
prop.expr label scale "1 + 0.05 * sig.pulse"
prop.expr label y "-120 + sin(t * 2) * 16"
effect.add label drop_shadow

clip.add path V4 0.5 2.5 arrow 'props={"shape":"triangle","width":260,"height":200,"fill":"#7bd88f","stroke":"#123018","strokeWidth":10}'
effect.add arrow threshold
EOF

# Effect parameters. Effects have numeric ids, so they are set in a second pass
# that looks them up in the saved project: "clip name, effect type, position in
# the clip's chain, parameter, value".
python3 - "$here" "$cli" <<'PY'
import json, subprocess, sys, os

here, cli = sys.argv[1], sys.argv[2]
params = {
    "layers.mfproj": [
        ("clip_a.mp4", "saturation", 0, "amount", 1.4),
        ("clip_b.mp4", "blur", 0, "radius", 18),
        ("clip_b.mp4", "vignette", 1, "amount", 0.6),
        ("clip_c.mp4", "mask", 0, "shape", "ellipse"),
        ("clip_c.mp4", "mask", 0, "width", 1500),
        ("clip_c.mp4", "mask", 0, "height", 1000),
        ("clip_c.mp4", "mask", 0, "feather", 220),
        ("clip_c.mp4", "brightness_contrast", 1, "brightness", 0.05),
        ("clip_c.mp4", "brightness_contrast", 1, "contrast", 1.2),
    ],
    "text_paths.mfproj": [
        ("clip_b.mp4", "blur", 0, "radius", 10),
        ("title", "drop_shadow", 0, "blur", 26),
        ("title", "drop_shadow", 0, "x", 8),
        ("title", "drop_shadow", 0, "y", 10),
        ("title", "drop_shadow", 0, "opacity", 0.7),
        ("sub", "tint", 0, "amount", 0.6),
        ("star", "drop_shadow", 0, "blur", 30),
        ("star", "drop_shadow", 0, "x", 10),
        ("star", "drop_shadow", 0, "y", 14),
        ("star", "drop_shadow", 0, "opacity", 0.8),
        ("ellipse", "drop_shadow", 0, "blur", 18),
        ("ellipse", "drop_shadow", 0, "x", -6),
        ("ellipse", "drop_shadow", 0, "y", -8),
        ("ellipse", "drop_shadow", 0, "opacity", 0.5),
    ],
    "generators.mfproj": [
        ("bars", "brightness_contrast", 0, "contrast", 1.3),
        ("bars", "vignette", 1, "amount", 0.5),
        ("rings", "invert", 0, "mix", 0.6),
        ("label", "drop_shadow", 0, "blur", 18),
        ("label", "drop_shadow", 0, "x", 6),
        ("label", "drop_shadow", 0, "y", 8),
        ("label", "drop_shadow", 0, "opacity", 0.6),
        ("arrow", "threshold", 0, "level", 0.45),
    ],
}

def send(path, lines):
    out = subprocess.run([cli, "--project", path, "-"], input="\n".join(lines),
                         capture_output=True, text=True)
    replies = [json.loads(line) for line in out.stdout.splitlines()]
    for reply in replies:
        if not reply.get("ok", False) or reply.get("errors"):
            sys.exit(f"{path}: {out.stdout}{out.stderr}")
    return replies

# A second clip on each of V2 and V3 in layers.mfproj, so each has a transition
# to run inside the two seconds the benchmark renders. Clips are addressed by
# id here, because the new clips repeat the media names.
layers = os.path.join(here, "layers.mfproj")
send(layers, ["clip.media clip_c.mp4 1.2 V2", "clip.media clip_a.mp4 1.2 V3"])
project = json.load(open(layers))
first, second = {}, {}
for c in project["clips"]:
    (second if abs(c["start"] - 1.2) < 1e-9 else first)[c.get("name", "")] = c["id"]
top_c, top_a = second["clip_c.mp4"], second["clip_a.mp4"]
send(layers, [f"clip.rename {top_c} top_c",
              f"clip.rename {top_a} top_a",
              f"prop.set {top_a} fit cover",
              f"transition.add {first['clip_b.mp4']} {top_c} wipe 0.8",
              f"transition.add {first['clip_c.mp4']} {top_a} dip 0.6"])
print("bench: added the transition pair to layers.mfproj")

for name in sorted(params):
    path = os.path.join(here, name)
    ids = {}
    for clip in json.load(open(path))["clips"]:
        for index, effect in enumerate(clip.get("videoEffects", [])):
            ids[(clip.get("name", ""), effect["type"], index)] = effect["id"]
    lines = []
    for clip_name, effect_type, index, prop, value in params[name]:
        key = (clip_name, effect_type, index)
        if key not in ids:
            sys.exit(f"{name}: no effect {key}")
        lines.append(f"prop.set {ids[key]} {prop} {value}")
    send(path, lines)
    print(f"bench: set {len(lines)} effect parameters in {name}")
PY

echo "bench: wrote $(ls "$here"/*.mfproj | wc -l | tr -d ' ') projects to $here"