globallycoherent RWStructuredBuffer<uint> Output;
[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    Output[0] = 1;
}
