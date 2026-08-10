struct StaticHelpers
{
    static float Identity(float value)
    {
        return value;
    }
};

RWStructuredBuffer<float> Output : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Output[0] = 1.0;
}
