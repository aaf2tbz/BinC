# sc: out = clamp(a*k, lo, hi) with k=2, lo=3, hi=15 (locals + for + if/else)
kernel sc
grid 4
buf 0 1.0 5.0 10.0 20.0
out 1 4
buf 2 2.0
buf 3 3.0
buf 4 15.0
expect 1 3.0 10.0 15.0 15.0
