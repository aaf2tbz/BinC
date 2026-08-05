# blend: out = a*0.5 + b*0.5
kernel blend
grid 4
buf 0 1.0 2.0 3.0 4.0
buf 1 10.0 20.0 30.0 40.0
out 2 4
expect 2 5.5 11.0 16.5 22.0
