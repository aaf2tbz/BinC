# h: half buffer, 2 halfs per 32-bit word (little-endian: element 0 = low 16 bits)
# a = {1.0, 2.0, 3.0, 4.0}  half bits: 1.0=0x3C00 2.0=0x4000 3.0=0x4200 4.0=0x4400
# word0 = 0x40003C00 = 1073757184 ; word1 = 0x44004200 = 1140867584
# out = {2.5, 4.5, 6.5, 8.5} half bits: 2.5=0x4100 4.5=0x4480 6.5=0x4680 8.5=0x4840
# word0 = 0x44804100 = 1149255936 ; word1 = 0x48404680 = 1212171904
# scale = half 2.0 = 0x4000, passed as the low 16 bits of a constant-buffer word
kernel h
grid 4
buf 0 1073757184 1140867584
out 1 2
buf 2 16384
expect 1 1149255936 1212171904
