# stencil: out[i+1] = in[i]*0.25 + in[i+1]*0.5 + in[i+2]*0.25 for i in 0..3
# in = 0 1 2 3 4 5  ->  out = 0 1 2 3 4 0 (edges untouched)
kernel stencil
grid 4
buf 0 0.0 1.0 2.0 3.0 4.0 5.0
out 1 6
expect 1 0.0 1.0 2.0 3.0 4.0 0.0
