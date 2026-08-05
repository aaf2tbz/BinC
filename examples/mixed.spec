# mx: one Mix element, byte-exact. in: f=1.5 i=-7 h=2.0 b=1 u=5 v=(3,4)
# expected: f=2.5 i=-6 h=4.0 b=0 u=4000000005 v=(6,8)
kernel mx
grid 1
bufh 0 00 00 C0 3F F9 FF FF FF 00 40 01 00 05 00 00 00 00 00 40 40 00 00 80 40
expecth 0 00 00 20 40 FA FF FF FF 00 44 00 00 05 28 6B EE 00 00 C0 40 00 00 00 41
