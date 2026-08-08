#!/bin/bash
# UEGroupsharedFixture golden: compile the compute kernel, spec, render, compare.
set -euo pipefail
cd "$(dirname "$0")/../../../.."   # repo root
HERE=tools/hlsl-diff/goldens/uecompute
SRC=$HERE/UEGroupsharedFixture.hlsl
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

timeout 120 ./binc/binc -E CSMain -T cs_5_0 "$SRC" -o "$TMP/out.metallib" 2>"$TMP/err.txt" || {
    echo "COMPILE FAIL:"; tail -3 "$TMP/err.txt"; exit 1; }
cp UEGroupsharedFixture.ll "$TMP/k.ll"

python3 "$HERE/spec.py" "$TMP/k.ll" > "$TMP/k.spec" 2>/dev/null

timeout 120 ./binc/harness "$TMP/out.metallib" "$TMP/k.spec" 2>/dev/null \
    | grep -E "^(buf1|OK|✅)" > "$TMP/actual.txt" || true

if [ ! -f "$HERE/expected.txt" ]; then
    cp "$TMP/actual.txt" "$HERE/expected.txt"
    echo "pinned golden -> $HERE/expected.txt"
else
    if diff -q "$HERE/expected.txt" "$TMP/actual.txt" >/dev/null; then
        echo "OK: UEGroupsharedFixture render matches the pinned golden"
    else
        echo "FAIL: render differs from the pinned golden"
        diff "$HERE/expected.txt" "$TMP/actual.txt" | head -5
        exit 1
    fi
fi
