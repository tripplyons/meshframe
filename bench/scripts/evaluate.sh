#!/usr/bin/env bash
# Scores a candidate: total deterministic render work units over the benchmark
# projects. Lower is better. One JSON object on stdout, diagnostics on stderr.
#
# Run from the worktree root.
set -euo pipefail

build=build-ar

[[ -f CMakeLists.txt ]] || { echo "run me from the worktree root" >&2; exit 1; }
log() { echo "[evaluate] $*" >&2; }

if ! git diff --quiet HEAD -- src/core/work_counter.hpp src/core/work_counter.cpp; then
  log "work counters changed; the metric must stay fixed"
  exit 1
fi

if [[ ! -f "$build/build.ninja" ]]; then
  log "configuring"
  cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DMESHFRAME_TESTS=ON -DMESHFRAME_BENCH=ON >&2
fi

log "building"
cmake --build "$build" >&2

if [[ ! -f bench/media/clip_a.mp4 ]]; then
  log "generating test videos with ffmpeg"
  bench/make-media.sh >&2
fi

log "rendering the benchmark projects"
"./$build/meshframe_bench" --backend cpu --frames 60 --json | python3 -c '
import json, sys
line = sys.stdin.read().strip()
bench = json.loads(line)
print(json.dumps({"aggregate": bench["aggregate"], "samples": bench["samples"]}))
'
