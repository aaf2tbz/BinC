#!/usr/bin/env python3
"""coverage-audit: D3D12/D3D11/D3D10 (+DXGI) coverage-gap audit.

Runs the BinC HLSL frontend over every corpus shader with an sm4+ profile
(vs_4_0+, ps_4_0+, cs_4_0+, hs_, ds_, gs_, as_, ms_) from both sample
families, classifies each (DXC reference accept / ours compile), buckets the
first frontend error into feature-gap categories, and emits a findings report.

Gate: any crash (rc 134/139) or hang (timeout) in the probe set fails.

usage: coverage-audit.py <out.md> [--limit N]
"""
import collections
import json
import os
import re
import subprocess
import sys

ROOT = "/Volumes/AverySSD/binc"
BINC = os.path.join(ROOT, "binc/binc")
DXC = os.path.join(ROOT, "third_party/DirectXShaderCompiler/build/bin/dxc")
ENV = dict(os.environ)
ENV["PATH"] = ENV.get("PATH", "") + ":" + os.path.dirname(DXC)


def run(cmd, timeout=40, cwd=None):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           cwd=cwd, env=ENV, errors="replace")
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "TIMEOUT"


def detect_stage(src):
    if "[numthreads" in src:
        if re.search(r"\[\s*shader\s*\(\s*\"?(amplification|mesh)", src):
            return "as/ms"
        return "cs"
    if re.search(r"TriangleStream|PointStream|LineStream|maxvertexcount", src):
        return "gs"
    if re.search(r"PatchConstant|OutputPatch|InputPatch|HullShader|DomainShader", src):
        return "hs/ds"
    if re.search(r"RaytracingAccelerationStructure|\[shader\(|raytracing", src):
        return "rt"
    if "SV_Target" in src or re.search(r":\s*COLOR0?\b", src):
        return "ps"
    if re.search(r":\s*POSITION\b", src):
        return "vs"
    return "?"


# bucket: (regex on the first error line, gap label)
GAP_BUCKETS = [
    (r"unsupported atomic method (\w+)", "buffer-method (RW/append %s)"),
    (r"geometry shader lowering", "geometry-shader (mesh stage)"),
    (r"undefined function (\w+)", "intrinsic/function gap (%s)"),
    (r"undefined name (\w+)", "undefined name (macro/include stripping)"),
    (r"expected an expression", "parse: expression syntax"),
    (r"expected semantic", "parse: semantic"),
    (r"subscript of non-pointer", "array/index typing"),
    (r"field access on non-struct", "struct/typing"),
    (r"vector width mismatch", "vector width typing"),
    (r"multiple inout params", "inout params"),
    (r"not a mutable local", "const/thread-id write"),
    (r"metal front-end failed", "codegen/AIR emit"),
    (r"struct type mismatch", "struct typing"),
    (r"unknown struct (\w+)", "struct (%s)"),
    (r"unknown type", "type"),
    (r"unsupported .* method", "object method"),
    (r"compute entry must return void", "entry signature"),
    (r"fragment output fields need", "PS-out semantics ([[color(N)]])"),
    (r"expected numthreads", "numthreads parse"),
    (r"entry point '[^']*' not found", "entry detection"),
    (r"internal: struct return", "struct return (include-stripped)"),
    (r"expected a type", "type parse"),
    (r"cannot open include", "include missing"),
    (r"expected ;", "parse: expected ;"),
    (r"unknown", "unknown name/type"),
]


def bucket_error(err):
    first = next((l.strip() for l in err.splitlines() if "error" in l), "")
    for rx, label in GAP_BUCKETS:
        m = re.search(rx, first)
        if m:
            if "%s" in label:
                return label % (m.group(1) if m.lastindex else "?")
            return label
    return ("other: " + first[:80]) if first else "other"


def guess_entry(src):
    """Heuristic entry: the first function whose params/return carry a stage
    semantic (SV_*, POSITION, COLOR, TEXCOORD...) and that is not a struct def."""
    for m in re.finditer(
            r"\b(?:void|float\d?|float\d+x\d+|uint\d?|int\d?|half\d?|bool|"
            r"[A-Za-z_]\w*)\s+(\w+)\s*\(([^)]*)\)", src):
        name, params = m.group(1), m.group(2)
        if name.startswith("(") or name in ("if", "for", "while", "return", "switch"):
            continue
        if re.search(r":\s*(SV_|POSITION|COLOR|TEXCOORD|NORMAL|TANGENT|BINORMAL)", params) or \
           re.search(r":\s*(SV_|POSITION|COLOR|TEXCOORD)", src[m.end():m.end() + 80]):
            return name
    m = re.search(r"\[numthreads[^\]]*\]\s*\n\s*void\s+(\w+)", src)
    if m:
        return m.group(1)
    return None


def main():
    outmd = sys.argv[1]
    limit = 0
    for a in sys.argv[2:]:
        if a.startswith("--limit="):
            limit = int(a.split("=")[1])

    curated = json.load(open(os.path.join(ROOT, "tools/vendor/sample_profiles.json")))
    d = json.load(open(os.path.join(ROOT, "tools/vendor/corpus.json")))
    jobs = []  # (relpath, profile, entry, family)
    seen = set()
    for path, info in curated.items():
        for i, prof in enumerate(info["profiles"]):
            entry = info["entries"][i] if i < len(info["entries"]) else info["entries"][0]
            key = (path, prof, entry)
            if key not in seen:
                seen.add(key)
                jobs.append((path, prof, entry, "curated"))
    def resolve(f):
        return f if os.path.exists(os.path.join(ROOT, f)) else os.path.join(ROOT, "third_party", f)

    for s in d["shaders"]:
        f = s["file"]
        if f.startswith("DirectXShaderCompiler/"):
            continue
        if f.startswith("UnrealEngine/"):
            continue  # UE corpus is the ue-audit.py phase-2 scope
        if f.startswith("DirectX-Graphics-Samples"):
            fam = "d3d12"
        elif f.startswith("DirectX-SDK-Samples"):
            fl = f.lower()
            if "direct3d11" in fl or "d3d11" in fl:
                fam = "d3d11"
            elif "direct3d10" in fl or "d3d10" in fl:
                fam = "d3d10"
            elif "dxgi" in fl:
                fam = "dxgi"
            else:
                fam = "d3d10/11"
        else:
            fam = "other"
        # stage/profile/entry detection: prefer the scanner's profiles, else
        # infer from the source (samples carry no embedded profiles)
        if not s["profiles"]:
            try:
                src = open(resolve(f), "rb").read().decode("utf-8", "replace")
            except OSError:
                continue
            stage = detect_stage(src)
            entry = guess_entry(src) or "main"
            # entry-name profile hints beat the file-level stage guess for
            # multi-stage files (VSMain must compile as vs_*, not ps_*)
            en = entry.lower()
            if en.startswith("vs") or en.startswith("vertex"):
                prof = "vs_5_0"
            elif en.startswith("ps") or en.startswith("frag") or en.startswith("pixel"):
                prof = "ps_5_0"
            elif en.startswith("cs") or en.startswith("compute"):
                prof = "cs_5_0"
            else:
                prof = {"vs": "vs_5_0", "ps": "ps_5_0", "cs": "cs_5_0",
                        "gs": "gs_5_0", "hs/ds": "hs_5_0", "as/ms": "as_5_0",
                        "rt": "lib_6_3"}.get(stage)
            if not prof:
                continue
            key = (f, prof, entry)
            if key not in seen:
                seen.add(key)
                jobs.append((f, prof, entry, fam))
            continue
        for p in s["profiles"]:
            if not re.match(r"(vs|ps|cs|hs|ds|gs|as|ms)_", p):
                continue
            if int(p.split("_")[1]) < 4 and p.split("_")[0] not in ("hs", "ds", "gs", "as", "ms"):
                continue
            entry = s["entries"][0] if s["entries"] else "main"
            key = (f, p, entry)
            if key not in seen:
                seen.add(key)
                jobs.append((f, p, entry, fam))
    if limit:
        jobs = jobs[:limit]

    fam_count = collections.Counter()
    stage_count = collections.Counter()
    gap_count = collections.Counter()
    gap_files = collections.defaultdict(list)
    fam_gap = collections.Counter()
    n_crash = 0
    rows = []
    no_dxc = os.environ.get("BINC_AUDIT_NO_DXC") == "1"
    import concurrent.futures as cf

    def one_job(args):
        rel, prof, entry, fam = args
        path = rel if os.path.exists(rel) else os.path.join(ROOT, "third_party", rel)
        if not os.path.exists(path):
            return None
        try:
            src = open(path, "rb").read().decode("utf-8", "replace")
        except OSError:
            return None
        stage = detect_stage(src)
        dxc_ok = False
        if not no_dxc:
            rc, _, _ = run([DXC, "-E", entry, "-T", prof, path, "-Fo", "/tmp/audit.dxil"], 30)
            dxc_ok = rc == 0
        rc, _, err = run([BINC, "-E", entry, "-T", prof, path, "-o", "/tmp/audit.metallib"], 40)
        if rc in (134, 139):
            result = "CRASH"
        elif rc == 124:
            result = "HANG"
        elif rc == 0:
            result = "COMPILES"
        else:
            result = "GAP:" + bucket_error(err)
        return (rel, prof, entry, fam, stage, dxc_ok, result)

    with cf.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for r in ex.map(one_job, jobs):
            if r is None:
                continue
            rel, prof, entry, fam, stage, dxc_ok, result = r
            if result == "CRASH" or result == "HANG":
                n_crash += 1
            elif result != "COMPILES":
                gap = result[4:]
                gap_count[gap] += 1
                if len(gap_files[gap]) < 3:
                    gap_files[gap].append(rel)
                fam_gap[(fam, gap)] += 1
            fam_count[fam] += 1
            stage_count[stage] += 1
            rows.append(r)

    with open(outmd, "w") as f:
        f.write("# HLSL sm4+ coverage audit — D3D12 / D3D11 / D3D10 (+DXGI)\n\n")
        f.write(f"Generated by `tools/hlsl-diff/coverage-audit.py`. {len(rows)} sm4+ jobs "
                f"({len(jobs)} queued, missing files skipped).\n")
        f.write("`COMPILES` = binc emits a metallib; `GAP:` rows are bucketed by the first\n")
        f.write("frontend error. **Any CRASH/HANG fails the audit.**\n\n")
        f.write("## Family × stage\n\n")
        f.write("| family | vs | ps | cs | gs | hs/ds | as/ms | rt | ? |\n|---|---|---|---|---|---|---|---|---|\n")
        fams = ["curated", "d3d12", "d3d11", "d3d10", "dxgi", "d3d10/11", "other"]
        for fam in fams:
            sub = [r for r in rows if r[3] == fam]
            if not sub:
                continue
            c = collections.Counter(r[4] for r in sub)
            f.write(f"| {fam} | {c.get('vs',0)} | {c.get('ps',0)} | {c.get('cs',0)} | "
                    f"{c.get('gs',0)} | {c.get('hs/ds',0)} | {c.get('as/ms',0)} | {c.get('rt',0)} | {c.get('?',0)} |\n")
        f.write("\n## Results by family\n\n")
        for fam in fams:
            sub = [r for r in rows if r[3] == fam]
            if not sub:
                continue
            res = collections.Counter(r[6].split(":")[0] for r in sub)
            f.write(f"- **{fam}** ({len(sub)}): " + ", ".join(f"{k}={v}" for k, v in res.most_common()) + "\n")
        f.write("\n## Feature-gap buckets (first error)\n\n")
        f.write("| gap | count | sample files |\n|---|---|---|\n")
        for gap, n in gap_count.most_common():
            f.write(f"| {gap} | {n} | {', '.join(gap_files[gap])} |\n")
        f.write("\n## Feature-gap buckets by family\n\n")
        fams2 = [fam for fam in ("curated", "d3d12", "d3d11", "d3d10", "dxgi", "d3d10/11", "other")
                 if any(f2 == fam for f2, _ in fam_gap)]
        for fam in fams2:
            sub = [(g, n) for (f2, g), n in fam_gap.items() if f2 == fam]
            if not sub:
                continue
            f.write(f"### {fam}\n\n")
            f.write("| gap | count |\n|---|---|\n")
            for g, n in sorted(sub, key=lambda kv: -kv[1]):
                f.write(f"| {g} | {n} |\n")
            f.write("\n")
        f.write("\n## Per-file rows\n\n")
        f.write("| file | profile | entry | family | stage | dxc | result |\n|---|---|---|---|---|---|---|\n")
        for rel, prof, entry, fam, stage, dxc_ok, result in rows:
            f.write(f"| {rel} | {prof} | {entry} | {fam} | {stage} | {'y' if dxc_ok else 'n'} | {result} |\n")
    print(f"audit: {len(rows)} jobs -> {outmd}")
    for gap, n in gap_count.most_common(12):
        print(f"  {n:3d}  {gap}")
    print(f"  compiles: {sum(1 for r in rows if r[6] == 'COMPILES')}")
    if n_crash:
        print(f"FAIL: {n_crash} crashes/hangs")
        sys.exit(1)
    print("no crashes/hangs")


if __name__ == "__main__":
    main()
