Texture2D<uint> PageTable : register(t0);
RWStructuredBuffer<uint> Counter : register(u0);
RWStructuredBuffer<float> Output : register(u1);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint loaded = PageTable.Load(int3(0, 0, 0));
    uint oldValue = 0;
    InterlockedAdd(Counter[0], loaded, oldValue);
    Output[0] = (float)oldValue;
}
