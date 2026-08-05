vertex vs
fragment fs
render 4 4 2
buf 0 -0.5 -0.5 0.0 1.0 0.5 -0.5 0.0 1.0 0.0 0.5 0.0 1.0
draw 3
expectpix 0 1 2 -0.25 -0.25 1.0 1.0
expectpix 0 2 2 0.25 -0.25 1.0 1.0
expectpix 1 1 2 1.5 2.5 0.0 1.0
expectpix 1 2 2 2.5 2.5 0.0 1.0
expectpix 0 0 0 0.0 0.0 0.0 0.0
expectdepth 1 2 0.5
expectdepth 2 2 0.5
expectdepth 0 0 1.0
