// Phase-5 atomic stress: 1024 threads race to increment one histogram
// counter via InterlockedAdd; the result must be exactly 1024.
RWStructuredBuffer<uint> counter : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    InterlockedAdd(counter[0], 1);
}
