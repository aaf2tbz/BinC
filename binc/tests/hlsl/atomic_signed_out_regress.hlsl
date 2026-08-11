// HLSL permits an int out result for a uint InterlockedAdd; both are
// 32-bit integer values in the AIR atomic ABI.
RWStructuredBuffer<uint> Counter : register(u0);

[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int Previous;
    InterlockedAdd(Counter[0], 1, Previous);
    Counter[1] = (uint)Previous;
}
