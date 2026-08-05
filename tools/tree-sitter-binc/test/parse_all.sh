#!/bin/bash
# tools/tree-sitter-binc/test/parse_all.sh — parse every example + the prelude
# with the generated tree-sitter parser; fail on any ERROR/MISSING node.
# Skips gracefully (exit 0) when the tree-sitter CLI is unavailable.
set -u
cd "$(dirname "$0")/.." || exit 1
if ! command -v npx >/dev/null 2>&1 || ! npx tree-sitter --version >/dev/null 2>&1; then
  echo "SKIP: tree-sitter CLI not available"
  exit 0
fi
fail=0; pass=0
for f in ../../examples/*.binc ../../binc/prelude.binc; do
  if npx tree-sitter parse "$f" 2>/dev/null | grep -q "ERROR\|MISSING"; then
    echo "FAIL: $f"; fail=$((fail+1))
  else
    pass=$((pass+1))
  fi
done
if [ "$fail" -eq 0 ]; then
  echo "tree-sitter: all $pass files parse cleanly"
  exit 0
fi
echo "tree-sitter: $fail file(s) failed"
exit 1
