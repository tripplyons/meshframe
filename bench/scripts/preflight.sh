#!/usr/bin/env bash
# Correctness gate for the render-speed campaign. Builds the tree in the
# campaign worktree, makes the test videos, runs the test suite, and renders the
# benchmark projects, which must be bit-identical to the pinned digests.
#
# Run from the worktree root. All logging goes to stderr; stdout stays empty.
set -euo pipefail

build=build-ar

[[ -f CMakeLists.txt ]] || { echo "run me from the worktree root" >&2; exit 1; }
log() { echo "[preflight] $*" >&2; }

# The counters define the score. Changing them would change the metric instead
# of the renderer, so a candidate may not touch them.
if ! git diff --quiet HEAD -- src/core/work_counter.hpp src/core/work_counter.cpp; then
  log "work counters changed; the metric must stay fixed"
  exit 1
fi

log "configuring"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMESHFRAME_TESTS=ON -DMESHFRAME_BENCH=ON >&2
cmake --build "$build" >&2

if [[ ! -f bench/media/clip_a.mp4 ]]; then
  log "generating test videos with ffmpeg"
  bench/make-media.sh >&2
fi

log "running the test suite"
"./$build/meshframe_tests" --gtest_brief=1 >&2

log "checking the rendered frames against the pinned digests"
"./$build/meshframe_bench" --frames 60 --expect bench/expected_digests.txt >&2

log "ok"