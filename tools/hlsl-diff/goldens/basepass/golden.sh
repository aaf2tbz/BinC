#!/bin/bash
# BasePassFeatureFixture golden: compile PS+VS, merge, spec, render, compare.
# Pins the pix0/pix1 render outputs (the fixture exercises packed cbuffers,
# LWC tile-offset structs, matrix by-value args, default params, int->bool
# args, clip->discard, SV_IsFrontFace, and empty stage-in structs).
set -euo pipefail
cd "$(dirname "$0")/../../../.."   # repo root
HERE=tools/hlsl-diff/goldens/basepass
SRC=$HERE/BasePassFeatureFixture.hlsl
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

run() {
    timeout 120 ./binc/binc -E "$1" -T "$2" "$SRC" -o "$TMP/out.metallib" 2>"$TMP/err.txt" || {
        echo "COMPILE FAIL ($1):"; tail -3 "$TMP/err.txt"; exit 1; }
}

run main ps_5_0
cp BasePassFeatureFixture.air "$TMP/ps.air"
cp BasePassFeatureFixture.ll "$TMP/ps.ll"
run mainVS vs_5_0
cp BasePassFeatureFixture.air "$TMP/vs.air"

xcrun metallib "$TMP/ps.air" "$TMP/vs.air" -o "$TMP/lib.metallib" 2>/dev/null

python3 "$HERE/spec.py" "$TMP/ps.ll" > "$TMP/lib.spec" 2>/dev/null

timeout 120 ./binc/harness "$TMP/lib.metallib" "$TMP/lib.spec" 2>/dev/null \
    | grep -E "^pix0:|^pix1:" > "$TMP/actual.txt"

if [ ! -f "$HERE/expected.txt" ]; then
    cp "$TMP/actual.txt" "$HERE/expected.txt"
    echo "pinned golden -> $HERE/expected.txt"
else
    if diff -q "$HERE/expected.txt" "$TMP/actual.txt" >/dev/null; then
        echo "OK: BasePassFeatureFixture render matches the pinned golden"
    else
        echo "FAIL: render differs from the pinned golden"
        diff "$HERE/expected.txt" "$TMP/actual.txt" | head -5
        exit 1
    fi
fi
