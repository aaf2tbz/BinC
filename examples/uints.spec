# u: unsigned /, %, < over values > 2^31 (bits survive the harness's int32 words)
kernel u
grid 2
buf 0 4000000000 7
out 1 2
out 2 2
out 3 2
expect 1 1333333333 2
expect 2 3 0
expect 3 0 1
