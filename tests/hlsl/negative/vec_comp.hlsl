RWStructuredBuffer<float> b : register(u0);
[numthreads(1,1,1)]
void main(uint3 t : SV_DispatchThreadID){ float2 v = float2(1,2); b[0] = v.z; }
