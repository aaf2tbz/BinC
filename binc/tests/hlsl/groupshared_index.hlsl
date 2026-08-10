groupshared int Values[4];
RWStructuredBuffer<float> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Values[0] = 7;
    Out[0] = (float)Values[0];
}
