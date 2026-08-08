#!/usr/bin/env python3
"""ue-audit: Unreal Engine SM6 corpus parse-acceptance audit.

Runs the BinC HLSL frontend over every UE .usf/.ush in corpus.json with the
ShaderCompilerWorker-parity define set, classifies each (COMPILES vs first
error bucketed into feature-gap categories), and emits ue-audit.md.

Gate: any crash (rc 134/139) or hang (timeout) fails.

usage: ue-audit.py <out.md> [--limit N]
"""
import collections
import json
import os
import re
import subprocess
import sys

ROOT = "/Volumes/AverySSD/binc"
BINC = os.path.join(ROOT, "binc/binc")
ENV = dict(os.environ)
UE_DIR = os.path.join(ROOT, "third_party/UnrealEngine")
UE_DEFS = ["-D", "COMPILER_DXC", "-D", "PLATFORM_WINDOWS", "-D", "SM6_PROFILE",
           "-D", "COMPILER_SUPPORTS_ATTRIBUTES", "-D", "A8_SAMPLE_MASK=.r",
           "-D", "WSVECTOR_IS_TILEOFFSET=1", "-D", "UE_LWC_RENDER_TILE_SIZE=2097152.0",
           "-D", "WORKING_COLOR_SPACE_RGB_TO_XYZ_MAT=float3x3(1.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0)"]


def resolve(rel):
    """corpus.json entries live under third_party/ (UnrealEngine/... ->
    third_party/UnrealEngine/...)."""
    return os.path.join(ROOT, "third_party", rel)


def run(cmd, timeout=60, cwd=None):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           cwd=cwd, env=ENV, errors="replace")
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "TIMEOUT"


GAP_BUCKETS = [
    (r"unsupported atomic method (\w+)", "buffer-method (RW/append %s)"),
    (r"geometry shader lowering", "geometry-shader (mesh stage)"),
    (r"undefined function (\w+)", "undefined function (%s)"),
    (r"undefined name (\w+)", "undefined name (missing include/define)"),
    (r"expected an expression", "parse: expression syntax"),
    (r"expected semantic", "parse: semantic"),
    (r"subscript of non-pointer", "array/index typing"),
    (r"field access on non-struct", "struct/typing"),
    (r"vector width mismatch", "vector width typing"),
    (r"vector operand in logical operator", "vector logical op"),
    (r"multiple inout params", "inout params"),
    (r"not a mutable local", "const/thread-id write"),
    (r"metal front-end failed", "codegen/AIR emit"),
    (r"struct type mismatch", "struct typing"),
    (r"unknown struct (\w+)", "unknown struct (%s)"),
    (r"unknown texture method (\w+)", "texture method (%s)"),
    (r"texture sample uv must be", "texture sample typing"),
    (r"unsupported .* method", "object method"),
    (r"compute entry must return void", "entry signature"),
    (r"fragment output fields need", "PS-out semantics"),
    (r"expected numthreads", "numthreads parse"),
    (r"entry point '[^']*' not found", "entry detection"),
    (r"internal: struct return", "struct return"),
    (r"expected a type", "type parse"),
    (r"expected global name", "global name parse"),
    (r"cannot open include", "include missing"),
    (r"cannot find include file", "include missing (generated)"),
    (r"expected ;", "parse: expected ;"),
    (r"unknown", "unknown name/type"),
]


def bucket_error(err):
    # the real failure is a "binc: error" line; "#error ..." directives come
    # out as warnings (UE config defines the audit does not pass) — skip them
    first = next((l.strip() for l in err.splitlines()
                  if "binc: error" in l and "#error" not in l), "")
    if not first:
        first = next((l.strip() for l in err.splitlines() if "error" in l), "")
    for rx, label in GAP_BUCKETS:
        m = re.search(rx, first)
        if m:
            if "%s" in label:
                return label % (m.group(1) if m.lastindex else "?")
            return label
    return ("other: " + first[:80]) if first else "other"


# Buckets that mean the PARSER itself failed (vs codegen-stage gaps after a
# successful parse). Parse acceptance = COMPILES + codegen-stage gaps.
PARSE_BUCKETS = {
    "entry detection",
    "type parse",
    "global name parse",
    "parse: expected ;",
    "parse: semantic",
    "numthreads parse",
    "geometry-shader (mesh stage)",
}


def guess_entry(src):
    """Heuristic entry for UE shaders: MainPS/MainVS/MainCS/CSMain first,
    else the first function with a stage semantic. Returns None for
    include-only headers (no plausible entry)."""
    for want in ("MainPS", "MainVS", "MainCS", "CSMain", "main"):
        if re.search(r"\b" + want + r"\s*\(", src):
            return want
    for m in re.finditer(
            r"\b(?:void|float\d?|float\d+x\d+|uint\d?|int\d?|half\d?|bool|"
            r"[A-Za-z_]\w*)\s+(\w+)\s*\(([^)]*)\)", src):
        name, params = m.group(1), m.group(2)
        if name.startswith("(") or name in ("if", "for", "while", "return", "switch"):
            continue
        if re.search(r":\s*(SV_|POSITION|COLOR|TEXCOORD|NORMAL|TANGENT)", params):
            return name
    m = re.search(r"\[numthreads[^\]]*\]\s*\n\s*void\s+(\w+)", src)
    if m:
        return m.group(1)
    return None


def entry_profile(entry):
    e = entry.lower()
    if e.startswith("mainps") or e.startswith("ps") or e.endswith("ps"):
        return "ps_5_0"
    if e.startswith("mainvs") or e.startswith("vs") or e.endswith("vs"):
        return "vs_5_0"
    if e.startswith("maincs") or e.startswith("cs") or e.endswith("cs"):
        return "cs_5_0"
    return "ps_5_0"


def main():
    outmd = sys.argv[1]
    limit = 0
    for a in sys.argv[2:]:
        if a.startswith("--limit="):
            limit = int(a.split("=")[1])

    d = json.load(open(os.path.join(ROOT, "tools/vendor/corpus.json")))
    jobs = []
    rows = []
    for s in d["shaders"]:
        f = s["file"]
        if not f.startswith("UnrealEngine/"):
            continue
        if not (f.endswith(".usf") or f.endswith(".ush")):
            continue
        try:
            src = open(resolve(f), "rb").read().decode("utf-8", "replace")
        except OSError:
            continue
        entry = guess_entry(src)
        prof = entry_profile(entry) if entry else "ps_5_0"
        jobs.append((f, prof, entry or "?", src))
    if limit:
        jobs = jobs[:limit]

    gap_count = collections.Counter()
    gap_files = collections.defaultdict(list)
    n_crash = 0
    for rel, prof, entry, src in jobs:
        # --stage-all: parse + lower every function, no entry required — the
        # parse-acceptance measurement (permutation-gated entries can't fail it)
        rc, _, err = run([BINC, "--stage-all", "-T", prof, "-I", UE_DIR] + UE_DEFS +
                         [resolve(rel), "-o", "/tmp/ue-audit.metallib"], 180)
        if rc in (134, 139):
            result = "CRASH"
            n_crash += 1
        elif rc == 124:
            result = "HANG"
            n_crash += 1
        elif rc == 0:
            result = "COMPILES"
        else:
            gap = bucket_error(err)
            result = "GAP:" + gap
            gap_count[gap] += 1
            if len(gap_files[gap]) < 3:
                gap_files[gap].append(rel)
        rows.append((rel, prof, entry, result))

    with open(outmd, "w") as f:
        f.write("# UE shader corpus audit — SM6 parse acceptance\n\n")
        f.write(f"Generated by `tools/hlsl-diff/ue-audit.py`. {len(rows)} UE shaders "
                f"({len(jobs)} queued). Defines: `{ ' '.join(UE_DEFS) }`.\n")
        f.write("`COMPILES` = binc emits a metallib; `GAP:` rows are bucketed by the first\n")
        f.write("frontend error. **Any CRASH/HANG fails the audit.**\n\n")
        res = collections.Counter(r[3].split(":")[0] for r in rows)
        f.write("## Results\n\n")
        f.write("| result | count |\n|---|---|\n")
        for k, v in res.most_common():
            f.write(f"| {k} | {v} |\n")
        f.write("\n## Feature-gap buckets (first error)\n\n")
        f.write("| gap | count | sample files |\n|---|---|---|\n")
        for gap, n in gap_count.most_common():
            f.write(f"| {gap} | {n} | {', '.join(gap_files[gap])} |\n")
        f.write("\n## Per-file rows\n\n")
        f.write("| file | profile | entry | result |\n|---|---|---|---|\n")
        for rel, prof, entry, result in rows:
            f.write(f"| {rel} | {prof} | {entry} | {result} |\n")
    print(f"ue-audit: {len(rows)} jobs -> {outmd}")
    for gap, n in gap_count.most_common(14):
        print(f"  {n:3d}  {gap}")
    ncomp = sum(1 for r in rows if r[3] == 'COMPILES')
    nparse = sum(1 for r in rows if r[3].startswith('GAP:') and r[3][4:] not in PARSE_BUCKETS and not r[3].startswith('GAP:other'))
    print(f"  compiles: {ncomp}")
    print(f"  parse acceptance (compiles + codegen-stage gaps): {ncomp + nparse}/{len(rows)} = {100.0*(ncomp+nparse)/max(len(rows),1):.1f}%")
    if n_crash:
        print(f"FAIL: {n_crash} crashes/hangs")
        sys.exit(1)
    print("no crashes/hangs")


if __name__ == "__main__":
    main()
