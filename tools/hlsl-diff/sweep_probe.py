#!/usr/bin/env python3
"""diff-sweep probe: corpus compute coverage table (Phase 8, plan item 1).

For every corpus [numthreads] shader:
  1. dxc accepts?  (reference path exists)
  2. binc compiles? (ours)
  3. if both: differential RUN via diff.sh (ours vs DXC->SPIRV-Cross on GPU)
Classifies each into a coverage table row (intrinsics / semantics / resources
/ result). Gate: NO CRASHES or HANGS (rc 134/139 or timeout) — any of those
fails the sweep; compile/mismatch rows are informational (unsupported
features are recorded with their first error, documented unsupported).

usage: sweep_probe.py <out.md> [--limit N]
"""
import json
import os
import re
import subprocess
import sys

ROOT = "/Volumes/AverySSD/binc"
BINC = os.path.join(ROOT, "binc/binc")
DXC = os.path.join(ROOT, "third_party/DirectXShaderCompiler/build/bin/dxc")
ENV = dict(os.environ)
ENV["PATH"] = ENV.get("PATH", "") + ":" + os.path.join(ROOT, "third_party/DirectXShaderCompiler/build/bin")

INTRINSIC_RE = re.compile(
    r"\b(mul|dot|normalize|cross|determinant|transpose|inverse|saturate|lerp|smoothstep|step|"
    r"pow|exp|exp2|log|log2|sqrt|rsqrt|abs|min|max|clamp|frac|floor|ceil|round|sign|"
    r"fmod|dot4|asfloat|asint|asuint|any|all|clip|tex2D|texCUBE|tex2Dproj|tex2Dlod|"
    r"Sample|SampleLevel|SampleBias|SampleCmp|Gather|Load|Store|InterlockedAdd|InterlockedMin|"
    r"GroupMemoryBarrier|GroupMemoryBarrierWithGroupSync|DeviceMemoryBarrier|WaveActiveSum|"
    r"WaveGetLaneCount|WaveGetLaneIndex|WaveActiveBallot|WavePrefixSum)\b")
SEM_RE = re.compile(r":\s*([A-Za-z][A-Za-z0-9_]*)\b")
RES_RE = re.compile(
    r"\b(RWStructuredBuffer|StructuredBuffer|RWTexture2D|RWTexture3D|Texture2D|Texture3D|"
    r"TextureCube|AppendStructuredBuffer|ConsumeStructuredBuffer|ByteAddressBuffer|"
    r"RWByteAddressBuffer|ConstantBuffer|cbuffer|sampler|SamplerState|SamplerComparisonState|"
    r"groupshared|raytracing|RaytracingAccelerationStructure|[A-Za-z_]+Buffer|[A-Za-z_]+Texture)\b")


def classify(src):
    ints = sorted(set(INTRINSIC_RE.findall(src)))
    sems = sorted(set(m for m in SEM_RE.findall(src) if m.isupper() and len(m) >= 3))
    res = sorted(set(RES_RE.findall(src)))
    return ints, sems, res


def run(cmd, timeout=30, cwd=None):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=cwd, env=ENV)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "TIMEOUT"


def main():
    outmd = sys.argv[1]
    limit = 0
    for a in sys.argv[2:]:
        if a.startswith("--limit="):
            limit = int(a.split("=")[1])
    # candidates: every corpus .hlsl with [numthreads]
    cands = []
    for base in ("DirectX-Graphics-Samples", "DirectX-SDK-Samples"):
        for dp, _, fns in os.walk(os.path.join(ROOT, "third_party", base)):
            for fn in fns:
                if not fn.endswith(".hlsl"):
                    continue
                path = os.path.join(dp, fn)
                try:
                    src = open(path, "rb").read().decode("utf-8", "replace")
                except OSError:
                    continue
                if "[numthreads" not in src:
                    continue
                rel = os.path.relpath(path, ROOT)
                # nBodyGravityCS has a dedicated differential (nbody-diff.sh,
                # part A of the sweep) with the fvk binding-shift flags; the
                # generic diff.sh invocation can't build its reference side.
                if rel.endswith("nBodyGravityCS.hlsl"):
                    continue
                m = re.search(r"\[numthreads[^\]]*\]\s*\n\s*void\s+(\w+)", src)
                entry = m.group(1) if m else "main"
                cands.append((rel, entry, src))
    if limit:
        cands = cands[:limit]

    rows = []
    n_crash = 0
    for rel, entry, src in cands:
        path = os.path.join(ROOT, rel)
        ints, sems, res = classify(src)
        row = {"file": rel, "entry": entry, "ints": ",".join(ints[:12]),
               "sems": ",".join(sems[:10]), "res": ",".join(res[:8]), "result": "", "detail": ""}
        rc, _, err = run([DXC, "-E", entry, "-T", "cs_5_0", path, "-Fo", "/tmp/sweep.dxil"], 30)
        if rc != 0:
            rc, _, err = run([DXC, "-E", entry, "-T", "cs_6_0", path, "-Fo", "/tmp/sweep.dxil"], 30)
            prof = "cs_6_0"
        else:
            prof = "cs_5_0"
        if rc in (134, 139):
            row["result"] = "DXC-CRASH"; row["detail"] = err.strip().splitlines()[-1][:100]
            n_crash += 1
            rows.append(row); continue
        if rc != 0:
            row["result"] = "DXC-REJECT"; rows.append(row); continue
        # ours compile
        rc, _, err = run([BINC, "-E", entry, "-T", prof, path, "-o", "/tmp/sweep.metallib"], 30)
        if rc in (134, 139):
            row["result"] = "CRASH"; row["detail"] = err.strip().splitlines()[-1][:100]
            n_crash += 1
            rows.append(row); continue
        if rc == 124:
            row["result"] = "HANG"; n_crash += 1
            rows.append(row); continue
        if rc != 0:
            first = next((l.strip() for l in err.splitlines() if "error" in l), err.strip().splitlines()[-1] if err.strip() else "")
            row["result"] = "OURS-REJECT"; row["detail"] = first[:120]
            rows.append(row); continue
        row["result"] = "COMPILES"
        # differential RUN: binc harness needs the buffer; run diff.sh (grid 16, 64 words)
        rc, out, err = run(["bash", os.path.join(ROOT, "tools/hlsl-diff/diff.sh"),
                            "-E", entry, "-T", prof, "-g", "16", "-w", "64", path], 120)
        if rc == 124:
            row["result"] = "DIFF-HANG"; n_crash += 1
        elif rc != 0:
            last = [l for l in out.splitlines() if l.strip()][-1] if out.strip() else err.strip().splitlines()[-1] if err.strip() else ""
            row["result"] = "DIFF-FAIL"; row["detail"] = last[:120]
        else:
            row["result"] = "DIFF-OK"
        rows.append(row)

    # coverage table
    with open(outmd, "w") as f:
        f.write("# HLSL differential sweep — corpus compute coverage\n\n")
        f.write("Generated by `tools/hlsl-diff/diff-sweep.sh` (probe: sweep_probe.py).\n")
        f.write("Rows: every corpus `[numthreads]` shader. `DIFF-OK` = pixel/word-identical\n")
        f.write("vs the DXC->SPIRV-Cross reference on GPU. `OURS-REJECT` rows carry the first\n")
        f.write("frontend error (feature gap, documented unsupported). **Any CRASH/HANG fails the gate.**\n\n")
        f.write("| file | entry | intrinsics | semantics | resources | result | detail |\n")
        f.write("|---|---|---|---|---|---|---|\n")
        for r in rows:
            f.write(f"| {r['file']} | {r['entry']} | {r['ints']} | {r['sems']} | {r['res']} | {r['result']} | {r['detail']} |\n")
        from collections import Counter
        cnt = Counter(r["result"] for r in rows)
        f.write("\n## Summary\n\n")
        for k, v in cnt.most_common():
            f.write(f"- {k}: {v}\n")
        f.write(f"\nTotal: {len(rows)} shaders.\n")
    print(f"coverage: {len(rows)} rows -> {outmd}")
    for k, v in Counter(r["result"] for r in rows).most_common():
        print(f"  {k}: {v}")
    if n_crash:
        print(f"FAIL: {n_crash} crashes/hangs in the probe set")
        sys.exit(1)
    print("no crashes/hangs in the probe set")


if __name__ == "__main__":
    main()
