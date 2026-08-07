#!/bin/sh
# Negative-test runner: every tests/negative/*.binc must FAIL to compile,
# and stderr must contain every expected substring from the sibling .expect
# file (one substring per line, '#' starts a comment line).
cd "$(dirname "$0")"
fail=0
for f in ../tests/negative/*.binc ../tests/negative/*.fx; do
    [ -e "$f" ] || continue
    case "$f" in
        *.fx) exp="${f%.fx}.expect";;
        *)    exp="${f%.binc}.expect";;
    esac
    if [ ! -e "$exp" ]; then
        echo "FAIL: $f has no .expect file"
        fail=1
        continue
    fi
    case "$f" in
        *.fx) out=$(./binc -E "$(basename "$f" .fx)" -T gs_4_0 --stage-all "$f" 2>&1);;
        *)    out=$(./binc "$f" 2>&1);;
    esac
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "FAIL: $f compiled successfully but should fail"
        fail=1
        continue
    fi
    ok=1
    while IFS= read -r want; do
        case "$want" in ''|'#'*) continue ;; esac
        case "$out" in
            *"$want"*) ;;
            *) echo "FAIL: $f stderr missing substring: $want"; ok=0; fail=1 ;;
        esac
    done < "$exp"
    [ $ok -eq 1 ] && echo "PASS: $f"
done
if [ $fail -eq 0 ]; then
    echo "ALL NEGATIVE TESTS PASS"
else
    exit 1
fi
