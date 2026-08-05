#!/bin/bash
# tools/hlsl-diff/corpus-parse.sh — DXC acceptance gate over the manifest.
#
# Compiles a curated set of corpus shaders with DXC (the reference toolchain):
#   - tools/vendor/sample_profiles.json: hand-maintained (profile, entry) map
#     for sample shaders whose build system drives -E/-T externally
#   - scanner-guessed sm4+ profiles from corpus.json as a fallback
# Reports pass/fail counts. This validates the manifest + reference toolchain;
# the BinC-frontend gate replaces the "ours" side in Phase 1.
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
jobs = []

# 1. curated profiles (authoritative where present)
curated = json.load(open("tools/vendor/sample_profiles.json"))
for path, info in curated.items():
    for i, prof in enumerate(info["profiles"]):
        entry = info["entries"][i] if i < len(info["entries"]) else info["entries"][0]
        jobs.append((path, prof, entry))

# 2. scanner guesses: sm4+ vs/ps/cs, excluding DXC's own test tree
d = json.load(open("tools/vendor/corpus.json"))
for s in d["shaders"]:
    if s["file"].startswith("DirectXShaderCompiler/") or s["file"] in curated:
        continue
    profs = [p for p in s["profiles"]
             if p.startswith(("vs_", "ps_", "cs_")) and int(p.split("_")[1]) >= 4]
    if not profs:
        continue
    entry = s["entries"][0] if s["entries"] else "main"
    jobs.append((s["file"], profs[0], entry))

for path, prof, entry in jobs:
    print(f"{path}\t{prof}\t{entry}")
EOF
while IFS=$'\t' read -r file profile entry; do
    [ -f "third_party/$file" ] && f="third_party/$file" || f="$file"
    [ -f "$f" ] || { skip=$((skip+1)); continue; }
    if [ "$LIMIT" -gt 0 ] && [ "$count" -ge "$LIMIT" ]; then break; fi
    count=$((count+1))
    if dxc -T "$profile" -E "$entry" "$f" -Fo /tmp/corpus.dxil >/dev/null 2>&1; then
        pass=$((pass+1))
    else
        fail=$((fail+1)); echo "$file ($profile, -E $entry)" >> /tmp/corpus_fail.txt
    fi
done < /tmp/corpus_jobs.txt
echo "corpus-parse: $pass passed, $fail failed, $skip skipped (${count} tried)"
if [ "$fail" -gt 0 ]; then echo "sample failures:"; head -5 /tmp/corpus_fail.txt; fi
exit 0   # informational in Phase 0; the BinC-side gate becomes hard in Phase 1
