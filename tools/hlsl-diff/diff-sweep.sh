#!/bin/bash
# tools/hlsl-diff/diff-sweep.sh — Phase 8 differential sweep gate.
#
# Proves the HLSL frontend at scale:
#   A. deterministic differential suite (HARD gate): every compute test in
#      tools/hlsl-diff/tests/compute/ via diff.sh, the full render suite
#      (corpus pairs + cbuffer + texture + MRT + matrices), and the corpus
#      nBodyGravity differential — all must pass.
#   B. corpus compute probe: every corpus [numthreads] shader classified
#      (DXC accept / ours compile / differential run) into a coverage table
#      (build/hlsl-diff/coverage.md) with intrinsics × semantics × resources.
#      Gate: no crashes or hangs in the probe set (unsupported-feature rows
#      are informational, each carrying its first frontend error).
#
# env: SWEEP_LIMIT=N  cap the probe set (debugging)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
WORK="build/hlsl-diff"
mkdir -p "$WORK"
FAIL=0

# ---- A. deterministic differential suite ----
echo "== compute differential suite"
for t in tools/hlsl-diff/tests/compute/*.hlsl; do
  echo "   $(basename "$t")"
  bash tools/hlsl-diff/diff.sh -g 16 -w 16 "$(pwd)/$t" >/dev/null 2>&1 || { echo "   FAIL: $t"; FAIL=1; }
done
echo "== render differential suite"
bash tools/hlsl-diff/render-suite.sh 2>&1 | tail -1 || FAIL=1
echo "== corpus compute differential (nBodyGravity)"
bash tools/hlsl-diff/nbody-diff.sh 2>&1 | tail -1 || FAIL=1

# ---- B. corpus compute probe + coverage table ----
LIMIT="${SWEEP_LIMIT:-0}"
ARGS=""
[ "$LIMIT" -gt 0 ] && ARGS="--limit=$LIMIT"
echo "== corpus compute probe"
python3 tools/hlsl-diff/sweep_probe.py "$WORK/coverage.md" $ARGS || FAIL=1

[ "$FAIL" = 0 ] && echo "diff-sweep: OK" || { echo "diff-sweep: FAILURES"; exit 1; }
