RWStructuredBuffer<float> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint mask = 0;
    Out[0] = !(mask & (1u << 7u)) ? 1.0 : 0.0;
}
