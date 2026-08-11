#include "forced_once_defs.hlsl"
MAKE_VEC(Output)
[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID) { }
