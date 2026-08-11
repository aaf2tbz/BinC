RWStructuredBuffer<uint> Out : register(u0);
static const float3 Weights = float3(0.25, 0.5, 0.75);
[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint lane = id.x % 3;
    Out[0] = (uint)(Weights[lane] * 100.0);
}
