# give_bone: Dog = {bones, happy, tail_wags}, treat = 1.0
# dog0: bones 0->1, joy=0.5+1=1.5 > 1 -> wags 0->10, happy=1
# dog1: bones 2->3, joy=0.2+1=1.2 > 1 -> wags 5->15, happy=1
kernel give_bone
grid 2
buf 0 0.0 0.5 0.0  2.0 0.2 5.0
buf 1 1.0
expect 0 1.0 1.0 10.0  3.0 1.0 15.0
