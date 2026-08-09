// HLSL modf returns the signed fractional part and writes trunc(x) through out.
RWStructuredBuffer<float> out_buf : register(u0);

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float x = float(tid.x) * 0.5 - 2.75;
    float integer_part;
    float fractional_part = modf(x, integer_part);
    float2 vector_integer;
    float2 vector_fractional = modf(float2(x, -x), vector_integer);
    out_buf[tid.x] = fractional_part + integer_part + vector_fractional.x + vector_integer.y;
}
