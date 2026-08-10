// HLSL unscoped enum constants lower as immutable integral module constants.
enum ETest : uint {
    ETest_A = (1 << 0),
    ETest_B = ETest_A << 1,
    ETest_C,
};

RWStructuredBuffer<float> Output : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float v = (float)ETest_C;
    Output[0] = v;
}
