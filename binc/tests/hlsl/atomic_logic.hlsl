RWStructuredBuffer<uint> Counter : register(u0);
RWStructuredBuffer<float> Output : register(u1);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint oldAnd = 0;
    uint oldOr = 0;
    InterlockedAnd(Counter[0], 3u, oldAnd);
    InterlockedOr(Counter[0], 4u, oldOr);
    Output[0] = (float)Counter[0] + (float)oldAnd + (float)oldOr;
}
