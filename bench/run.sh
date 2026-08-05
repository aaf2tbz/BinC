#!/bin/bash
# bench/run.sh — sanity-check AIR emission quality: compile the same fractal
# kernel from BinC and from hand-written MSL, dispatch both through the same
# harness, and report wall times. Requires a real Metal GPU (like make verify).
set -u
cd "$(dirname "$0")" || exit 1
DEV="${DEV:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}"
mkdir -p build

echo "== compiling fractal.binc (BinC -> AIR -> metallib)"
../binc/binc fractal.binc -o build/fractal.binc.metallib || exit 1

echo "== compiling fractal.metal (hand-written MSL)"
DEVELOPER_DIR="$DEV" xcrun metal -c fractal.metal -o build/fractal.metal.air || exit 1
DEVELOPER_DIR="$DEV" xcrun metallib build/fractal.metal.air -o build/fractal.metal.metallib || exit 1

echo "== dispatching (5 runs each; includes per-run pipeline setup)"
for lib in build/fractal.binc.metallib build/fractal.metal.metallib; do
  t0=$(python3 -c 'import time; print(time.time())')
  for i in 1 2 3 4 5; do
    ../binc/harness "$lib" fractal.spec >/dev/null 2>&1 || echo "harness failed on $lib"
  done
  t1=$(python3 -c 'import time; print(time.time())')
  ms=$(python3 -c "print(int(($t1-$t0)*1000))")
  echo "$(basename $lib): $ms ms for 5 dispatches (256x256 grid)"
done
