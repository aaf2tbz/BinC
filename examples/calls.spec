# ck: scaled(x,2) = avg2(x, 2x) + triple(2) = 1.5x + 6 ; s = 2.0 ; a[1] = 4.0
# thread0: (3+6)*2 + 4 = 22 ; thread1: (6+6)*2 + 4 = 28
kernel ck
grid 2
buf 0 2.0 4.0
out 1 2
buf 2 2.0
expect 1 22.0 28.0
