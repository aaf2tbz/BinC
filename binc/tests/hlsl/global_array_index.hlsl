static float3 Values[3];
RWStructuredBuffer<float> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint index = 1;
    Values[index] = float3(4.0, 5.0, 6.0);
    float3 value = Values[index];
    Out[0] = value.x + value.y + value.z;
}
