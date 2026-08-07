RWStructuredBuffer<float> b : register(u0);
[numthreads(1,1,1)]
void main(uint3 t : SV_DispatchThreadID){ float3 v = float3(1,2,3); b[0] = v.wxyz.x; }
