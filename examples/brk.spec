# bk: thread0 (a=1.0) accumulates 1+1+1+1+1 = 5.0; thread1 (a=2.0) stops at 6.0
kernel bk
grid 2
buf 0 1.0 2.0
out 1 2
expect 1 5.0 6.0
