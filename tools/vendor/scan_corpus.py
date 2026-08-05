#!/usr/bin/env python3
"""tools/vendor/scan_corpus.py — index the vendored DirectX corpus.

Walks third_party/ and emits tools/vendor/corpus.json:
  - shaders: every .hlsl/.fx/.fxh with best-guess profiles
    (from [shader("...")] attributes and `compile vs_3_0 ...` in .fx files)
  - textures: every .dds (and other texture formats) found
  - lit: the DXC HLSL lit-test corpus root + file count (parser conformance)
  - vendored: pinned commits per vendor
  - toolchain: versions of dxc / spirv-cross

Regenerate with `make corpus-scan`. Never edits anything it indexes.
"""
import datetime, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TP = os.path.normpath(os.path.join(HERE, "..", "..", "third_party"))
OUT = os.path.join(HERE, "corpus.json")

SHADER_EXTS = (".hlsl", ".fx", ".fxh")
TEX_EXTS = (".dds", ".ktx", ".tga", ".png", ".bmp", ".jpg")
LIT_ROOT = os.path.join("DirectXShaderCompiler", "tools", "clang", "test", "HLSL")

FX_COMPILE = re.compile(r"compile\s+(vs|ps|cs|gs|hs|ds)_(\d+)_(\d+)\s+(\w+)")
SHADER_ATTR = re.compile(r'\[shader\("(\w+)"\)\]')
STAGE_FROM_ATTR = {"vertex": "vs", "fragment": "ps", "compute": "cs",
                   "geometry": "gs", "hull": "hs", "domain": "ds", "mesh": "ms", "amplification": "as"}


def git_head(repo):
    try:
        out = subprocess.run(["git", "-C", os.path.join(TP, repo), "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=20)
        return out.stdout.strip()[:12] if out.returncode == 0 else "unknown"
    except Exception:
        return "unknown"


def tool_version(name, args):
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=30)
        return (out.stdout + out.stderr).splitlines()[0].strip()[:80]
    except Exception:
        return "not found"


def main():
    if not os.path.isdir(TP):
        print("third_party/ missing — run tools/vendor/clone.sh first", file=sys.stderr)
        return 1

    shaders, textures, lit_count = [], [], 0
    lit_root_abs = os.path.join(TP, LIT_ROOT)

    for dirpath, dirnames, filenames in os.walk(TP):
        dirnames[:] = [d for d in dirnames if not d.startswith(".git")]
        for fn in sorted(filenames):
            p = os.path.join(dirpath, fn)
            rel = os.path.relpath(p, TP)
            if rel.startswith(LIT_ROOT):
                if fn.endswith(SHADER_EXTS):
                    lit_count += 1
                continue
            if fn.endswith(SHADER_EXTS):
                profiles, entries, attrs, attr_stages = [], [], [], ""
                try:
                    txt = open(p, "rb").read().decode("utf-8", "replace")
                except OSError:
                    continue
                for m in FX_COMPILE.finditer(txt):
                    profiles.append(f"{m.group(1)}_{m.group(2)}_{m.group(3)}")
                    entries.append(m.group(4))
                for a in SHADER_ATTR.findall(txt):
                    attrs.append(a)
                    s = STAGE_FROM_ATTR.get(a)
                    if s:
                        attr_stages = s
                if not profiles and attr_stages:
                    profiles = [f"{attr_stages}_5_0"]
                if not profiles and fn.endswith(".fx"):
                    profiles = ["fx_2_0"]  # legacy placeholder; refined by hand later
                shaders.append({
                    "file": rel,
                    "ext": fn.rsplit(".", 1)[-1],
                    "profiles": sorted(set(profiles)),
                    "entries": sorted(set(entries)),
                    "shader_attr": attrs,
                })
            elif fn.endswith(TEX_EXTS):
                textures.append(rel)

    doc = {
        "generated": datetime.datetime.now().isoformat(timespec="seconds"),
        "vendored": {
            "DirectX-Graphics-Samples": {"commit": git_head("DirectX-Graphics-Samples")},
            "DirectX-SDK-Samples": {"commit": git_head("DirectX-SDK-Samples")},
            "DirectXShaderCompiler": {"commit": git_head("DirectXShaderCompiler")},
            "ShaderConductor": {"commit": git_head("ShaderConductor")},
        },
        "toolchain": {
            "dxc": tool_version("dxc", ["dxc", "--version"]),
            "spirv-cross": tool_version("spirv-cross", ["spirv-cross", "--version"]),
        },
        "lit": {"root": LIT_ROOT, "count": lit_count} if os.path.isdir(lit_root_abs) else None,
        "shaders": shaders,
        "textures": textures,
        "counts": {"shaders": len(shaders), "textures": len(textures), "lit": lit_count},
    }
    with open(OUT, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"corpus.json: {len(shaders)} shaders, {len(textures)} textures, {lit_count} lit tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
