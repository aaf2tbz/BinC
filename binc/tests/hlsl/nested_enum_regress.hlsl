struct NestedEnumHolder
{
    enum EMode : uint
    {
        Zero = 0,
        One = 1,
        Two = One + 1
    };
};

RWStructuredBuffer<float> Output : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Output[0] = (float)NestedEnumHolder::Two;
}
