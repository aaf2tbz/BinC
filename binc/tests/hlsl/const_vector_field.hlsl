static const float3 kConstVector = float3(1.0, 2.0, 3.0);

RWStructuredBuffer<float> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Out[0] = kConstVector.x + kConstVector.y + kConstVector.z;
}