// Regression for block-scope static/const qualifiers. Static locals are
// per-invocation AIR locals; const remains write-protected.
RWStructuredBuffer<uint> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    static const float Scale = 2.0;
    static const int Digits[3] = { 1, 2, 3 };
    Out[0] = (uint)(Scale * Digits[tid.x % 3]);
}
