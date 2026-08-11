// Focused regression for HLSL all(boolN): reduce a comparison mask to one bool.
RWStructuredBuffer<uint> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float3 value = float3(1.0, 2.0, 3.0);
    bool3 mask = value > 0.0;
    Out[0] = all(mask) ? 1u : 0u;
}
