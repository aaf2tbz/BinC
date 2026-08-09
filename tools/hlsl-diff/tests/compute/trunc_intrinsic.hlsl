// HLSL trunc rounds toward zero for scalar and vector lanes.
RWStructuredBuffer<float> out_buf : register(u0);
[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float value = float(tid.x) * 0.5 - 3.75;
    float2 vector_value = float2(value, -value - 0.25);
    float2 result = trunc(vector_value);
    out_buf[tid.x] = trunc(value) + result.x + result.y;
}
