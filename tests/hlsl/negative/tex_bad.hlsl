texture t;
sampler s;
RWStructuredBuffer<float> b : register(u0);
[numthreads(1,1,1)]
void main(uint3 t : SV_DispatchThreadID){ b[0] = tex2D(s, float2(0.5,0.5)); }
