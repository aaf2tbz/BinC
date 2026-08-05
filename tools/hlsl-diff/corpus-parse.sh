#!/bin/bash
# tools/hlsl-diff/corpus-parse.sh — DXC acceptance gate over the manifest.
#
# For every shader in tools/vendor/corpus.json that has a vs/ps/cs profile,
# compile it with DXC using that profile (the reference toolchain). Reports
# pass/fail counts and the worst offenders. This validates the corpus
# manifest + the reference toolchain; the BinC-frontend gate replaces the
# "ours" side in Phase 1.
#
# env: CORPUS_LIMIT=N  cap the number of files tried (default: all)
set -u
cd "$(dirname "$0")/../.." || exit 1
export PATH="$PATH:/Volumes/AverySSD/binc/third_party/DirectXShaderCompiler/build/bin"
command -v dxc >/dev/null 2>&1 || { echo "SKIP: dxc not on PATH"; exit 0; }
[ -f tools/vendor/corpus.json ] || { echo "SKIP: corpus.json missing (make corpus-scan)"; exit 0; }

LIMIT="${CORPUS_LIMIT:-0}"
pass=0; fail=0; skip=0; : > /tmp/corpus_fail.txt
count=0
python3 - <<'EOF' > /tmp/corpus_jobs.txt
import json
d = json.load(open("tools/vendor/corpus.json"))
for s in d["shaders"]:
    profs = [p for p in s["profiles"] if p.startswith(("vs_", "ps_", "cs_"))]
    if not profs:
        continue
    entry = s["entries"][0] if s["entries"] else "main"
    print(f"{s['file']}\t{profs[0]}\t{entry}")
EOF
while IFS=$'\t' read -r file profile entry; do
    [ -f "third_party/$file" ] || { skip=$((skip+1)); continue; }
    if [ "$LIMIT" -gt 0 ] && [ "$count" -ge "$LIMIT" ]; then break; fi
    count=$((count+1))
    if dxc -T "$profile" -E "$entry" "third_party/$file" -Fo /tmp/corpus.dxil >/dev/null 2>&1; then
        pass=$((pass+1))
    else
        fail=$((fail+1)); echo "$file ($profile, -E $entry)" >> /tmp/corpus_fail.txt
    fi
done < /tmp/corpus_jobs.txt
echo "corpus-parse: $pass passed, $fail failed, $skip skipped (${count} tried)"
if [ "$fail" -gt 0 ]; then echo "sample failures:"; head -5 /tmp/corpus_fail.txt; fi
exit 0   # informational in Phase 0; the BinC-side gate becomes hard in Phase 1
