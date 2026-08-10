#include "/Engine/Generated/UniformBuffers/View.ush"

RWStructuredBuffer<float> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Out[0] = View.OneOverPreExposure;
}
