#!/usr/bin/env python3
"""UEGroupsharedFixture spec generator (compute path).

Derives the buffer indices from the AIR metadata. The kernel has the Params
cbuffer (index 0) and the Out RW buffer (index 1); the groupshared array is a
module global (no binding). Writes the Params words (Center = 0.25) and the
Out buffer as zeros.

Usage: spec.py ours.ll > uecs.spec
"""
import re
import struct
import sys


def main():
    ll = open(sys.argv[1] if len(sys.argv) > 1 else "ours.ll").read()
    # kernel entry arglist: !N = !{<fnptr>, !<empty>, !<args>}
    arglist_node = None
    for line in ll.split("\n"):
        m = re.match(r'!(\d+) = !\{.*@CSMain.*?, !(\d+), !(\d+)\}$', line)
        if m:
            arglist_node = int(m.group(3))
            break
    if arglist_node is None:
        sys.exit("no kernel arglist found")
    al = re.search(r'!%d = !\{([^}]*)\}' % arglist_node, ll)
    arg_ids = [int(x) for x in re.findall(r'!(\d+)', al.group(1))]
    bufs = {}
    for i, aid in enumerate(arg_ids):
        node = re.search(r'!%d = !\{([^}]*)\}' % aid, ll)
        if not node:
            continue
        body = node.group(1)
        if '"air.buffer"' in body:
            nm = re.search(r'!"air\.arg_name", !"([^"]+)"', body)
            bufs[i] = nm.group(1) if nm else "?"
    out = []
    out.append("kernel CSMain")
    out.append("grid 64 1 1")
    out.append("dump 1")
    for idx in sorted(bufs):
        nm = bufs[idx]
        if nm == "Params":
            # Center float3 = 0.25, Count = 64
            w = [0] * 8
            for i in range(3):
                w[i] = struct.unpack("<I", struct.pack("<f", 0.25))[0]
            w[4] = 64
            out.append(f"buf {idx} " + " ".join(str(x) for x in w))
        else:
            out.append(f"buf {idx} " + " ".join(["0"] * 4))
    sys.stdout.write("\n".join(out) + "\n")
    sys.stderr.write(f"spec written ({list(bufs.items())})\n")


if __name__ == "__main__":
    main()
