RWStructuredBuffer<float> b : register(u0);
[numthreads(1,1,1)]
void main(uint3 t : SV_DispatchThreadID){ float2 a = float2(1,2); float3 c = a + float3(1,2,3); b[0] = c.x; }
