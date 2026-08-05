# chains: vs = {1,2}, {3,4}, {5,6} -> vs[0].x + vs[1].y*2 + vs[2].x = 1 + 8 + 5 = 14
kernel chains
grid 2
buf 0 1.0 2.0 3.0 4.0 5.0 6.0
out 1 2
expect 1 14.0 14.0
