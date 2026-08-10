RWStructuredBuffer<float> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int4 data = int4(0, 0, 0, 0);
    uint index = 2;
    data[index] = 7;
    Out[0] = (float)data[index];
}
