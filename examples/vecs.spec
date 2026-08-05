# vc: v = px*2 + splat(1), then v.y += 10, written back to px
# thread0: px (1,2,3,4) -> v (3,15,7,9)  ; out = 3+15+7+9 + 3*4 + 3 = 49
# thread1: px (5,6,7,8) -> v (11,23,15,17); out = 11+23+15+17 + 12 + 3 = 81
kernel vc
grid 2
buf 0 1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0
out 1 2
expect 0 3.0 15.0 7.0 9.0 11.0 23.0 15.0 17.0
expect 1 49.0 81.0
