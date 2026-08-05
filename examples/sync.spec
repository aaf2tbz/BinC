# sy: out = a * 2 after a threadgroup barrier
kernel sy
grid 4
buf 0 1.0 2.0 3.0 4.0
out 1 4
expect 1 2.0 4.0 6.0 8.0
