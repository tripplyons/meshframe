#!/usr/bin/env bash
# Builds and runs the whole test suite. The campaign's automatic gate is the
# frame digest in bench/expected_digests.txt; run this when a change touches
# behavior the benchmark projects do not cover.
#
# Run from the repository root.
set -euo pipefail

build="${1:-build-ar}"
launcher=()
command -v ccache >/dev/null 2>&1 && launcher=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache)

cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMESHFRAME_TESTS=ON -DMESHFRAME_BENCH=ON "${launcher[@]}" >&2
cmake --build "$build" >&2
"./$build/meshframe_tests" "$@"