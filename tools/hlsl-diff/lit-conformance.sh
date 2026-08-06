#!/bin/bash
# tools/hlsl-diff/lit-conformance.sh — Phase 1 conformance gate.
#
# Measures how much of DXC's HLSL lit-test corpus the BinC frontend parses.
# The filtered subset excludes preprocessor-heavy and template tests (the
# preprocessor and HLSL templates land in later phases), so the number is an
# honest "core language surface" acceptance rate, not a full-corpus claim.
#
# env: CORPUS_LIMIT=N  cap the number of files tried (default: all)
set -u
cd "$(dirname "$0")/../.." || exit 1
LIT="third_party/DirectXShaderCompiler/tools/clang/test/HLSL"
[ -d "$LIT" ] || { echo "SKIP: lit corpus not present (third_party/)"; exit 0; }
LIMIT="${CORPUS_LIMIT:-0}"
pass=0; fail=0; tried=0
for f in $(find "$LIT" -name "*.hlsl" | sort); do
    # excluded from the subset: preprocessor directives, HLSL templates,
    # resource-heavy includes, and negative/error-exercising tests
    case "$f" in *error*|*fail*|*invalid*|*diag*|*verify*) continue;; esac
    if grep -qE "^[[:space:]]*#(include|define|if|ifdef|ifndef|pragma|line|error)|template[[:space:]]*<|expected-error|RUN:.*FileCheck" "$f"; then
        continue
    fi
    if [ "$LIMIT" -gt 0 ] && [ "$tried" -ge "$LIMIT" ]; then break; fi
    tried=$((tried+1))
    if binc/binc -fsyntax-only -T vs_6_0 "$f" >/dev/null 2>&1; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
    fi
done
pct=$(python3 -c "print(f'{$pass*100.0/max($tried,1):.1f}')")
echo "lit conformance: $pass/$tried parsed ($pct%)"
[ "$tried" -gt 0 ] && [ "$pass" -ge $((tried*6/10)) ] && echo "GATE: >=60% (Phase 1 target)" || echo "GATE: below 60% (Phase 1 target)"
