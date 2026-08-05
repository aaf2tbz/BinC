# sl: q = (x+1, 2y) -> out = (q.y, q.x)
# thread0: in (1,2) -> q (2,4) -> out (4,2) ; thread1: in (5,6) -> q (6,12) -> out (12,6)
kernel sl
grid 2
buf 0 1.0 2.0 5.0 6.0
out 1 4
expect 1 4.0 2.0 12.0 6.0
