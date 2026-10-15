#!/usr/bin/env bash
# Run after building: examples/path-animation.sh [OUT_DIR]
# MESHFRAME_CLI selects an alternate executable.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cli="${MESHFRAME_CLI:-$root/build/meshframe-cli}"
out="${1:-$root/demo-out/paths}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
project="$out/paths.mfproj"
if [[ -e "$project" ]]; then
    echo "Project already exists: $project. Choose a new output directory." >&2
    exit 1
fi

"$cli" --project "$project" - <<EOF | while IFS= read -r reply; do
settings width=640 height=360 fps=30 background="#101820"
path.import "$root/examples/paths.svg" V1 0 6 route
prop.key route trimEnd 0 0
prop.key route trimEnd 2 1 ease-in-out

# Motion paths use local pixels relative to the frame center.
clip.add path V2 0 6 traveler 'props={"shape":"ellipse","width":26,"height":26,"fill":"#ffca67","motionPath":"M -240 30 C -140 -110 -60 150 40 0 S 180 -100 240 -30"}'
prop.key traveler motionProgress 0 0
prop.key traveler motionProgress 6 1 ease-in-out
effect.add traveler drop_shadow

# Morph a closed triangle into a closed rounded shape. Fill and stroke
# remain independently editable and react to the usual effects.
clip.add path V3 0 3 emblem 'props={"shape":"triangle","width":70,"height":70,"x":0,"y":-105,"fill":"#65d5d0","targetPath":"M 0 -35 A 35 35 0 1 1 0 35 A 35 35 0 1 1 0 -35 Z","stroke":"#ffffff","strokeWidth":2}'
prop.key emblem morph 0.5 0
prop.key emblem morph 2.5 1 ease-in-out
clip.add text V3 3 3 caption 'props={"text":"Paths in motion","size":28,"y":-105}'
transition.add emblem caption crossfade 0.8
sheet "$out/paths-sheet.png" count=9 width=320 columns=3
frame "$out/paths.png" 2
check
EOF
    if [[ "$reply" != *'"ok":true'* || "$reply" == *'"errors":[{'* ]]; then
        echo "path-animation: $reply" >&2
        exit 1
    fi
done

"$cli" --project "$project" export "$out/paths.mp4" wait=true >/dev/null
echo "Wrote $project, paths.png, paths-sheet.png, and paths.mp4 in $out"
