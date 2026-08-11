RWStructuredBuffer<float4> Out : register(u0);

[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float3 V = log10(float3(1.0, 10.0, 100.0));
    Out[0] = float4(log10(10.0), V);
}
