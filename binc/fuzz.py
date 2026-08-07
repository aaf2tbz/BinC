#!/usr/bin/env python3
"""Mutation fuzz smoke test for the binc compiler.

Two modes:
  .binc — seeds from ../examples/*.binc, `binc <file>` (the C-like frontend)
  .hlsl — seeds from ../tools/hlsl-diff/tests/compute/*.hlsl + a corpus
          compute shader, `binc -E main -T cs_5_0 <file> -o out.metallib`
          (the HLSL frontend, full pipeline incl. the metal front-end)

Every run must exit 0 (mutant still compiles) or 1 (clean compile error);
a crash (signal rc) or hang (timeout) fails the fuzz. Outputs go to a
private temp dir and are discarded.

Environment overrides: FUZZ_ITERS (default 300), FUZZ_SEED (default fixed),
FUZZ_HLSL_ITERS (default 100).
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
HLSL_SEEDS = glob.glob(os.path.join(HERE, "..", "tools", "hlsl-diff", "tests",
                                    "compute", "*.hlsl"))
CORPUS_CS = os.path.join(HERE, "..", "third_party", "DirectX-Graphics-Samples",
                         "Samples", "Desktop", "D3D12nBodyGravity", "src",
                         "nBodyGravityCS.hlsl")
if os.path.exists(CORPUS_CS):
    HLSL_SEEDS.append(CORPUS_CS)
ITERS = int(os.environ.get("FUZZ_ITERS", "300"))
HITERS = int(os.environ.get("FUZZ_HLSL_ITERS", "100"))
random.seed(int(os.environ.get("FUZZ_SEED", "20260805")))

CHARS = "(){}=;*+-/<>&|!.,:[]'\" \\n#"
HLSL_CHARS = CHARS + "@[]~"


def mutate(s: str, chars: str = CHARS) -> str:
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
        return s[:i] + random.choice(chars) + s[i + 1:]
    return s[:i] + random.choice(chars) + s[i:]  # insert


def run_one(cmd, td, it, seed_name):
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=15, cwd=td)
    except subprocess.TimeoutExpired:
        print(f"fuzz[{it}]: TIMEOUT on mutant of {seed_name}")
        return 1
    for f in ("fuzz.ll", "fuzz.air", "fuzz.metallib", "fuzz.h"):
        p = os.path.join(td, f)
        if os.path.exists(p):
            os.remove(p)
    if r.returncode not in (0, 1):
        print(f"fuzz[{it}]: CRASH rc={r.returncode} on mutant of {seed_name}")
        print(r.stderr.decode(errors="replace")[:400])
        return 1
    return 0


def main():
    crashes = 0
    with tempfile.TemporaryDirectory() as td:
        # ---- .binc mode (the C-like frontend keeps running) ----
        if SEEDS:
            for it in range(ITERS):
                seed = random.choice(SEEDS)
                src = open(seed).read()
                mutant = mutate(mutate(src))
                path = os.path.join(td, "fuzz.binc")
                with open(path, "w") as fh:
                    fh.write(mutant)
                crashes += run_one([BINC, path], td, it, os.path.basename(seed))
        # ---- .hlsl mode (the DirectX frontend) ----
        if HLSL_SEEDS:
            for it in range(HITERS):
                seed = random.choice(HLSL_SEEDS)
                src = open(seed).read()
                mutant = mutate(mutate(src), HLSL_CHARS)
                path = os.path.join(td, "fuzz.hlsl")
                with open(path, "w") as fh:
                    fh.write(mutant)
                crashes += run_one([BINC, "-E", "main", "-T", "cs_5_0", path,
                                    "-o", os.path.join(td, "fuzz.metallib")],
                                   td, it, os.path.basename(seed))
    total = ITERS + HITERS
    if crashes:
        print(f"fuzz: {crashes} crash(es) out of {total} iterations — FAIL")
        return 1
    print(f"fuzz: {total} iterations clean (no crashes, no hangs; "
          f"{ITERS} .binc + {HITERS} .hlsl)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
