#!/usr/bin/env python3
"""Normalmap golden spec generator.

BumpDirectionalLight.fx (GDC 2005 HLSL workshop, inlined sas.fxh struct).
$uniforms layout (from the .ll): { i32 GlobalParameter, World, View, Projection,
SasDirectionalLight{float3 Direction; float4 Color}, i1 UseNormalMaps,
float reflectionRatio, float SpecularRatio, float SpecularStyleLerp,
i32 SpecularPower } — 272 bytes, bound at buffer 11 (VS+PS).

NOTE on the harness word format: every FLOAT must be spliced as raw bits
(tokens without a '.' are int32-bits); a literal "1" would splice as 1.4e-45
and clip the triangle (position w ~ 0).
"""
import re
import struct

def f(v): return str(struct.unpack('<I', struct.pack('<f', float(v)))[0])
def i(v): return str(int(v))

def air_resources(ll, func):
    """Return (uniforms_buf, [(tex_loc, is_cube), ...]) for `func`, parsed from
    the AIR metadata. Binding indices shift when the argument layout changes
    (selective resource forwarding drops dead args), so derive, don't hardcode."""
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
        return None, []
    refs = re.findall(r"!(\d+)", nodes[fn])
    if not refs:
        return None, []
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
    return uidx, texs

def main(ll_path, spec_path):
    # vertex data: pos(4) nrm(3) tang(3) binorm(3) uv(2) @ 0/16/28/40/52, stride 60
    verts = []
    def add(p, n, t, b, u):
        verts.extend(p); verts.extend(n); verts.extend(t); verts.extend(b); verts.extend(u)
    add([-0.5,-0.5,0.0,1.0],[0.0,0.0,1.0],[1.0,0.0,0.0],[0.0,1.0,0.0],[1.0,0.0])
    add([ 0.0, 0.5,0.0,1.0],[0.0,0.0,1.0],[1.0,0.0,0.0],[0.0,1.0,0.0],[0.0,1.0])
    add([ 0.5,-0.5,0.0,1.0],[0.0,0.0,1.0],[1.0,0.0,0.0],[0.0,1.0,0.0],[0.0,0.0])
    vwords = [f(v) for v in verts]

    cbuf = [0]*68
    cbuf[0] = 1
    cbuf[4:20] = [1.0,0,0,0, 0,1.0,0,0, 0,0,1.0,0, 0,0,0,1.0]
    cbuf[20:36] = [1.0,0,0,0, 0,1.0,0,0, 0,0,1.0,0, 0,0,0,1.0]
    cbuf[36:52] = [1.0,0,0,0, 0,1.0,0,0, 0,0,1.0,0, 0,0,0,1.0]
    cbuf[52:55] = [0.0, 0.0, -1.0]
    cbuf[56:60] = [1.0, 1.0, 1.0, 1.0]
    cbuf[60] = 1
    cbuf[61] = 0.1
    cbuf[62] = 0.15
    cbuf[63] = 0.15
    cbuf[64] = 32
    cwords = [i(v) if idx in (0, 60, 64) else f(v) for idx, v in enumerate(cbuf)]

    ll = open(ll_path).read()
    vs_u, _ = air_resources(ll, "VS")
    ps_u, ps_texs = air_resources(ll, "PS")
    vs_idx = vs_u if vs_u is not None else 11
    ps_idx = ps_u if ps_u is not None else 11
    texlines = "\n".join(
        f"tex {loc} 64 64{' cube' if cube else ''}" for loc, cube in ps_texs)
    buflines = "\n".join(
        f"buf {idx} {' '.join(cwords)}"
        for idx in dict.fromkeys([vs_idx, ps_idx]))  # dedupe, keep order
    spec = f"""vertex VS
fragment PS
render 64 64 1
draw 3
vtx 0 1 0 float4
vtx 4 1 16 float3
vtx 5 1 28 float3
vtx 6 1 40 float3
vtx 1 1 52 float2
{texlines}
buf 1 {' '.join(vwords)}
{buflines}
dumppix 0
"""
    open(spec_path, 'w').write(spec)
    print(f"spec written ({len(cwords)} cbuf words, {len(vwords)} vert words; vs cbuf={vs_idx} ps cbuf={ps_idx})")

if __name__ == '__main__':
    import sys
    main(sys.argv[1], sys.argv[2])
