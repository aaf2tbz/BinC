#!/usr/bin/env python3
"""BasePassFeatureFixture spec generator.

Derives the resource binding indices from the emitted AIR metadata (the
golden-d3d9 lesson: indices shift across compiler changes as selective
resource forwarding drops dead args — never hardcode them). The fixture is a
fragment-only render: two constant buffers (Primitive, View) plus the
implicit __uniforms struct.

Usage: spec.py ours.ll > basepass.spec
"""
import re
import sys


def air_resources(lltext, func):
    """Return ([(arg_index, is_cube)], {arg_index: name}) for the function's
    buffer/texture/sampler args from the AIR metadata nodes."""
    # !N = !{i32 <idx>, !"air.buffer", ... !"air.arg_name", !"<name>"}
    nodes = {}
    for m in re.finditer(r'!(\d+) = !\{i32 (\d+), !"air\.(buffer|texture|sampler)",[^}]*?"air\.arg_name", !"([^"]+)"\}', lltext):
        nodes[int(m.group(2))] = (m.group(3), m.group(4))
    return nodes


def main():
    ll = open(sys.argv[1] if len(sys.argv) > 1 else "ours.ll").read()
    # fragment entry (the only stage): find its metadata arglist
    frag = re.search(r'!air\.fragment = !\{!(\d+)\}', ll)
    if not frag:
        sys.exit("no fragment entry in AIR")
    # node -> arg list: !N = !{<fnptr>, !<outs>, !<args>} (fnptr may contain
    # struct-literal braces, so match per-line by the trailing !ref pair)
    arglist_node = None
    for line in ll.split("\n"):
        m = re.match(r'!(\d+) = !\{.*@main.*?, !(\d+), !(\d+)\}$', line)
        if m:
            arglist_node = int(m.group(3))
            break
    if arglist_node is None:
        sys.exit("no fragment arglist found")
    # the arglist: !N = !{!a, !b, ...}
    al = re.search(r'!%d = !\{([^}]*)\}' % arglist_node, ll)
    arg_ids = [int(x) for x in re.findall(r'!(\d+)', al.group(1))]
    # each arg node's buffer index -> arg index mapping
    bufs = {}
    for i, aid in enumerate(arg_ids):
        node = re.search(r'!%d = !\{([^}]*)\}' % aid, ll)
        if not node:
            continue
        body = node.group(1)
        if '"air.buffer"' in body:
            m = re.search(r'!"air\.arg_type_name", !"([^"]+)"', body)
            nm = re.search(r'!"air\.arg_name", !"([^"]+)"', body)
            bufs[i] = (m.group(1) if m else "?", nm.group(1) if nm else "?")
    # cbuffer field words: parse the cbuffer struct defs from the AIR type table
    structs = {}
    for m in re.finditer(r'%struct\.([A-Za-z0-9_$]+) = type \{([^}]*)\}', ll):
        fields = [f.strip() for f in m.group(2).split(",")]
        structs[m.group(1)] = fields
    # word counts per cbuffer: Primitive$cb and View$cb and $uniforms
    def words_of(tag):
        if tag not in structs:
            return 0
        tot = 0
        for f in structs[tag]:
            mm = re.match(r'\[(\d+) x (.+)\]', f)
            if mm:
                n, elt = int(mm.group(1)), mm.group(2)
                w = 1 if elt == "float" else int(re.match(r'<(\d+)', elt).group(1)) if elt.startswith("<") else 1
                tot += n * w
            elif f.startswith("<"):
                w = int(re.match(r'<(\d+)', f).group(1))
                tot += w
            elif f.startswith("%struct."):
                tot += 4  # nested struct: pad to a conservative 4 words
            else:
                tot += 1
        return tot

    out = []
    out.append("vertex mainVS")
    out.append("fragment main")
    out.append("render 16 16 2")
    out.append("draw 3")
    out.append("dumppix 0")
    out.append("dumppix 1")
    out.append("vtx 0 2 0 float4")
    # fullscreen triangle (clip space) at buffer 2, separate from the cbufs
    import struct
    verts = [(-1.0, -1.0, 0.0, 1.0), (3.0, -1.0, 0.0, 1.0), (-1.0, 3.0, 0.0, 1.0)]
    vwords = []
    for v in verts:
        for c in v:
            vwords.append(struct.unpack("<I", struct.pack("<f", c))[0])
    out.append("buf 2 " + " ".join(str(w) for w in vwords))
    # buffers in metadata order
    for idx in sorted(bufs):
        tyname, nm = bufs[idx]
        tag = nm + "$cb" if nm == "Primitive" or nm == "View" else "$uniforms"
        nwords = words_of(tag)
        words = [0] * nwords
        if nm == "Primitive":
            # LocalToWorld (first 16 words) = identity; ObjectBoundsMin 0.25
            for i in range(4):
                words[i * 4 + i] = 1.0
            for i in range(16, 19):
                words[i] = 0.25
        elif nm == "View":
            # ViewToClip (first 16 words) = identity; ViewForward = (0,0,1)
            for i in range(4):
                words[i * 4 + i] = 1.0
            if len(words) > 19:
                words[19] = 0.0
                words[20] = 0.0
                words[21] = 1.0
        words = " ".join(str(struct.unpack("<I", struct.pack("<f", w))[0]) if isinstance(w, float) else str(w) for w in words)
        out.append(f"buf {idx} {words}")
    sys.stdout.write("\n".join(out) + "\n")
    sys.stderr.write(f"spec written ({len(bufs)} buffers; {[ (i, bufs[i][1]) for i in sorted(bufs) ]})\n")


if __name__ == "__main__":
    main()
