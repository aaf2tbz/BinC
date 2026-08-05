# bench — AIR emission quality sanity check

The same kernel written twice — once in BinC (`fractal.binc`) and once in
hand-written MSL (`fractal.metal`) — compiled to `.metallib` and dispatched
through the same harness. The point is not micro-benchmarking; it is a sanity
check that BinC's AIR emission lands in the same performance class as a
hand-written equivalent (fast-math flags, vector ops, control flow).

Run:

    ./run.sh          # needs a real Metal GPU (like make verify)

The harness dispatch includes per-run pipeline creation on both sides, so the
comparison is apples-to-apples (and compile time is reported separately by the
script output).
