#include "../hlsl/norm_path_shared.hlsl"

RWStructuredBuffer<float> Output : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    Output[0] = NormPathValue;
}
