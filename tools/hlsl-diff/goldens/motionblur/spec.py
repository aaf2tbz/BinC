#!/usr/bin/env python3
"""PixelMotionBlur (D3D9 SDK) golden spec generator — 2-pass technique.

Parses %struct.$uniforms from the emitted .ll, computes the LLVM byte layout,
fills the field values, and writes a harness spec for ONE pass (the runner
invokes us once per pass with the pass's fragment entry).

Usage: spec.py <ours.ll> <out.spec> [vs_entry] [ps_entry]

Technique WorldWithVelocityTwoPasses:
  P0 = WorldVertexShader + WorldPixelShaderColor  (mesh color = tex2D x diffuse)
  P1 = WorldVertexShader + WorldPixelShaderVelocity (velocity = posCurr-posLast)/2

Hand-verified with identity mWorld/mWVP/mWVPLast:
  light = max(0, dot((0,0,1), normalize(1,1,1))) = 0.577350
  P0 color = texel(x+1, y+1, x+y+1, 1) * (1*1*0.57735 + 0.1*1) = texel * 0.67735
  P1 velocity = (0, 0, 1, 1) everywhere (current == last frame).
"""
import re
import struct
import sys

LL = re.compile(r"%struct\.\$uniforms = type \{ (.*?) \}")

SIZE = {"float": 4, "i32": 4, "i1": 1, "half": 2}
ALIGN = {"float": 4, "i32": 4, "i1": 1, "half": 2}


def field_layout(typestr):
    t = typestr.strip()
    m = re.fullmatch(r"<(\d+) x (\w+)>", t)
    if m:
        n, elt = int(m.group(1)), m.group(2)
        sz = n * SIZE[elt]
        al = 1
        while al < sz:
            al *= 2
        return sz, max(al, 4 if elt != "i1" else 1)
    m = re.fullmatch(r"\[(\d+) x (.*)\]", t)
    if m:
        n = int(m.group(1))
        es, ea = field_layout(m.group(2))
        return n * es, ea
    if t in SIZE:
        return SIZE[t], ALIGN[t]
    raise ValueError(f"unknown field type {t!r}")


def struct_layout(body):
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


def air_resources(ll, func):
    """Return (found, uniforms_buf, [(tex_loc, is_cube), ...]) for `func`,
    parsed from the AIR metadata. Binding indices shift when the argument
    layout changes (selective resource forwarding drops dead args), so derive,
    don't hardcode."""
    nodes = {}
    for line in ll.splitlines():
        m2 = re.match(r"!(\d+) = !\{(.*)\}\s*$", line.strip())
        if m2:
            nodes[int(m2.group(1))] = m2.group(2)
    fn = None
    for nid, body in nodes.items():
        if re.search(r"@" + re.escape(func) + r"\b", body) and body.lstrip().startswith("<"):
            fn = nid
            break
    if fn is None:
        return False, None, []
    refs = re.findall(r"!(\d+)", nodes[fn])
    if not refs:
        return True, None, []
    arglist = nodes.get(int(refs[-1]), "")
    uidx = None
    texs = []
    for aid in (int(x) for x in re.findall(r"!(\d+)", arglist)):
        ab = nodes.get(aid, "")
        if "air.buffer" in ab and '"__uniforms"' in ab:
            m3 = re.search(r'"air.location_index", i32 (\d+)', ab)
            if m3:
                uidx = int(m3.group(1))
        elif "air.texture" in ab:
            m3 = re.search(r'"air.location_index", i32 (\d+)', ab)
            if m3:
                texs.append((int(m3.group(1)), "texturecube" in ab))
    return True, uidx, texs


def main():
    llpath, specpath = sys.argv[1], sys.argv[2]
    vs = sys.argv[3] if len(sys.argv) > 3 else "VS"
    ps = sys.argv[4] if len(sys.argv) > 4 else "PS"
    ll = open(llpath).read()
    m = LL.search(ll)
    if not m:
        print("no $uniforms struct in .ll"); sys.exit(1)
    offsets, total = struct_layout(m.group(1))
    buf = bytearray(total)

    FIELD_ORDER = [
        "MaterialAmbientColor", "MaterialDiffuseColor", "LightDir",
        "LightAmbient", "LightDiffuse", "mWorld", "mWorldViewProjection",
        "mWorldViewProjectionLast", "PixelBlurConst", "ConvertToNonHomogeneous",
        "VelocityCapSq", "RenderTargetWidth", "RenderTargetHeight",
    ]

    def put(name, values):
        off = offsets[FIELD_ORDER.index(name)]
        for v in values:
            if isinstance(v, tuple):
                val, kind = v
                if kind == "i":
                    buf[off:off + 4] = struct.pack("<i", val); off += 4
                else:
                    buf[off] = val & 0xFF; off += 1
            else:
                buf[off:off + 4] = struct.pack("<f", float(v)); off += 4

    ident = [1.0, 0.0, 0.0, 0.0,
             0.0, 1.0, 0.0, 0.0,
             0.0, 0.0, 1.0, 0.0,
             0.0, 0.0, 0.0, 1.0]
    ldir = 1.0 / 3.0 ** 0.5  # normalize(1,1,1)
    VALUES = {
        "MaterialAmbientColor": [0.1, 0.1, 0.1, 1.0],
        "MaterialDiffuseColor": [1.0, 1.0, 1.0, 1.0],
        "LightDir": [ldir, ldir, ldir, 0.0],
        "LightAmbient": [1.0, 1.0, 1.0, 1.0],
        "LightDiffuse": [1.0, 1.0, 1.0, 1.0],
        "mWorld": ident,
        "mWorldViewProjection": ident,
        "mWorldViewProjectionLast": ident,
        "PixelBlurConst": [1.0],
        "ConvertToNonHomogeneous": [0.0],
        "VelocityCapSq": [1.0],
        "RenderTargetWidth": [64.0],
        "RenderTargetHeight": [64.0],
    }
    for name in FIELD_ORDER:
        if name in VALUES:
            put(name, VALUES[name])

    words = [struct.unpack("<I", buf[i:i + 4])[0] for i in range(0, total, 4)]
    cbuf = " ".join(str(w) for w in words)
    vs_found, vs_u, _ = air_resources(ll, vs)
    ps_found, ps_u, ps_texs = air_resources(ll, ps)
    vs_idx = vs_u if vs_u is not None else (8 if vs_found else None)
    ps_idx = ps_u if ps_u is not None else (8 if ps_found else None)
    texlines = "\n".join(
        f"tex {loc} 64 64{' cube' if cube else ''}" for loc, cube in ps_texs)
    if not texlines and not ps_found:
        texlines = "tex 1 64 64"  # historical fallback
    buflines = "\n".join(
        f"buf {idx} {' '.join(str(w) for w in words)}"
        for idx in dict.fromkeys(i for i in (vs_idx, ps_idx) if i is not None))
    # vertex: pos(4) nrm(3) uv(2) @ 0/16/28, stride 36 — flat triangle, normal +Z
    spec = f"""vertex {vs}
fragment {ps}
render 64 64 1
draw 3
vtx 0 1 0 float4
vtx 4 1 16 float3
vtx 1 1 28 float2
{texlines}
buf 1 -0.5 -0.5 0.0 1.0 0.0 0.0 1.0 1.0 0.0 -0.0 0.5 0.0 1.0 0.0 0.0 1.0 0.0 1.0 0.5 -0.5 0.0 1.0 0.0 0.0 1.0 0.0 0.0
{buflines}
dumppix 0
"""
    open(specpath, "w").write(spec)
    print(f"spec written ({len(words)} cbuf words, {total} bytes; {vs} + {ps})")


if __name__ == "__main__":
    main()
