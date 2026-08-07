#!/usr/bin/env python3
"""BasicHLSL golden spec generator.

Parses %struct.$uniforms from the emitted .ll, computes the LLVM byte layout,
fills the field values below, and writes a harness spec. CBUF words are
emitted as raw int tokens (the harness splices int tokens as raw 32-bit
patterns) so mixed float/bool/i32 words carry the exact bytes.
"""
import re
import struct
import sys

LL = re.compile(r"%struct\.\$uniforms = type \{ (.*?) \}")

SIZE = {"float": 4, "i32": 4, "i1": 1, "half": 2}
ALIGN = {"float": 4, "i32": 4, "i1": 1, "half": 2}


def field_layout(typestr):
    """Return (size, align) for one LLVM field type string."""
    t = typestr.strip()
    m = re.fullmatch(r"<(\d+) x (\w+)>", t)
    if m:
        n, elt = int(m.group(1)), m.group(2)
        sz = n * SIZE[elt]
        # target datalayout: v16:16:16 v24:32:32 v32:32:32 v48:64:64 v64:64:64
        # -> vector ABI align rounds the byte size up to a power of two
        al = 1
        while al < sz:
            al *= 2
        return sz, max(al, 4 if elt != "i1" else 1)
    m = re.fullmatch(r"\[(\d+) x (.*)\]", t)
    if m:
        n = int(m.group(1))
        es, ea = field_layout(m.group(2))
        # LLVM array: element-aligned, total = n*es, align = ea
        return n * es, ea
    if t in SIZE:
        return SIZE[t], ALIGN[t]
    raise ValueError(f"unknown field type {t!r}")


def struct_layout(body):
    """LLVM struct layout: natural alignment per field."""
    fields = [f.strip() for f in body.split(",")]
    off = 0
    maxalign = 1
    out = []
    for f in fields:
        sz, al = field_layout(f)
        off = (off + al - 1) & ~(al - 1)
        out.append(off)
        off += sz
        maxalign = max(maxalign, al)
    total = (off + maxalign - 1) & ~(maxalign - 1)
    return out, total


def main():
    llpath, specpath = sys.argv[1], sys.argv[2]
    ll = open(llpath).read()
    m = LL.search(ll)
    if not m:
        print("no $uniforms struct in .ll"); sys.exit(1)
    offsets, total = struct_layout(m.group(1))
    buf = bytearray(total)

    def put(name, values):
        # values: list of floats/ints; ints marked via (v, 'i') tuples
        off = offsets[FIELD_ORDER.index(name)]
        for v in values:
            if isinstance(v, tuple):  # (value, 'i' | 'b')
                val, kind = v
                if kind == "i":
                    buf[off:off + 4] = struct.pack("<i", val)
                    off += 4
                else:
                    buf[off] = val & 0xFF
                    off += 1
            else:
                buf[off:off + 4] = struct.pack("<f", float(v))
                off += 4

    FIELD_ORDER = [
        "g_MaterialAmbientColor", "g_MaterialDiffuseColor", "g_nNumLights",
        "g_LightDir", "g_LightDiffuse", "g_LightAmbient", "g_fTime",
        "g_mWorld", "g_mWorldViewProjection", "nNumLights", "bTexture", "bAnimate",
    ]
    ident = [1.0, 0.0, 0.0, 0.0,
             0.0, 1.0, 0.0, 0.0,
             0.0, 0.0, 1.0, 0.0,
             0.0, 0.0, 0.0, 1.0]
    VALUES = {
        "g_MaterialAmbientColor": [0.2, 0.2, 0.2, 1.0],
        "g_MaterialDiffuseColor": [1.0, 0.8, 0.6, 1.0],
        "g_nNumLights": [(1, "i")],
        "g_LightDir": [0.408248, 0.408248, 0.816497, 0.0] + [0.0] * 8,
        "g_LightDiffuse": [1.0, 1.0, 1.0, 1.0] + [0.0] * 8,
        "g_LightAmbient": [0.15, 0.15, 0.15, 1.0],
        "g_fTime": [0.0],
        "g_mWorld": ident,
        "g_mWorldViewProjection": ident,
        "nNumLights": [(1, "i")],
        "bTexture": [(1, "b")],
        "bAnimate": [(0, "b")],
    }
    for name in FIELD_ORDER:
        if name in VALUES:
            put(name, VALUES[name])

    words = [struct.unpack("<I", buf[i:i + 4])[0] for i in range(0, total, 4)]
    cbuf = " ".join(str(w) for w in words)
    spec = f"""vertex RenderSceneVS
fragment RenderScenePS
render 64 64 1
draw 3
vtx 0 1 0 float4
vtx 4 1 16 float3
vtx 1 1 28 float2
tex 3 64 64
buf 1 -0.5 -0.5 0.0 1.0 0.0 0.0 1.0 1.0 0.0 -0.0 0.5 0.0 1.0 0.0 0.0 1.0 0.0 1.0 0.5 -0.5 0.0 1.0 0.0 0.0 1.0 0.0 0.0
buf 2 {cbuf}
buf 5 {cbuf}
dumppix 0
"""
    open(specpath, "w").write(spec)
    print(f"spec written ({len(words)} cbuf words, {total} bytes)")


if __name__ == "__main__":
    main()
