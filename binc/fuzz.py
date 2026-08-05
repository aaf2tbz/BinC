#!/usr/bin/env python3
"""Mutation fuzz smoke test for the binc compiler.

Borrows seeds from ../examples/*.binc, applies small character-level
mutations, and requires that the compiler never crashes (signal) or hangs:
every run must exit 0 (mutant still compiles) or 1 (clean compile error).
Outputs are written to a private temp dir and discarded.

Environment overrides: FUZZ_ITERS (default 300), FUZZ_SEED (default fixed).
"""
import glob
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BINC = os.path.join(HERE, "binc")
SEEDS = glob.glob(os.path.join(HERE, "..", "examples", "*.binc"))
ITERS = int(os.environ.get("FUZZ_ITERS", "300"))
random.seed(int(os.environ.get("FUZZ_SEED", "20260805")))

CHARS = "(){}=;*+-/<>&|!.,:[]'\" \\n"


def mutate(s: str) -> str:
    if not s:
        return "x"
    i = random.randrange(len(s))
    op = random.randrange(5)
    if op == 0:      # delete
        return s[:i] + s[i + 1:]
    if op == 1:      # duplicate
        return s[:i] + s[i] + s[i:]
    if op == 2:      # swap adjacent
        if i + 1 >= len(s):
            return s
        return s[:i] + s[i + 1] + s[i] + s[i + 2:]
    if op == 3:      # replace
        return s[:i] + random.choice(CHARS) + s[i + 1:]
    return s[:i] + random.choice(CHARS) + s[i:]  # insert


def main():
    if not SEEDS:
        print("fuzz: no seed files found", file=sys.stderr)
        return 1
    crashes = 0
    with tempfile.TemporaryDirectory() as td:
        for it in range(ITERS):
            seed = random.choice(SEEDS)
            src = open(seed).read()
            mutant = mutate(mutate(src))  # two mutations per round
            path = os.path.join(td, "fuzz.binc")
            with open(path, "w") as fh:
                fh.write(mutant)
            try:
                r = subprocess.run([BINC, path], capture_output=True, timeout=10,
                                   cwd=td)
            except subprocess.TimeoutExpired:
                print(f"fuzz[{it}]: TIMEOUT on mutant of {os.path.basename(seed)}")
                crashes += 1
                continue
            for f in ("fuzz.ll", "fuzz.air", "fuzz.metallib", "fuzz.h"):
                p = os.path.join(td, f)
                if os.path.exists(p):
                    os.remove(p)
            if r.returncode not in (0, 1):
                print(f"fuzz[{it}]: CRASH rc={r.returncode} on mutant of "
                      f"{os.path.basename(seed)}")
                print(r.stderr.decode(errors="replace")[:400])
                crashes += 1
    if crashes:
        print(f"fuzz: {crashes} crash(es) out of {ITERS} iterations — FAIL")
        return 1
    print(f"fuzz: {ITERS} iterations clean (no crashes, no hangs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
