// HLSL sincos writes sine and cosine through out parameters.
RWStructuredBuffer<float> out_buf : register(u0);

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float phase = float(tid.x) * 0.125;
    float s, c;
    sincos(phase, s, c);
    float2 vector_phase = float2(phase, phase + 0.5);
    float2 vector_s, vector_c;
    sincos(vector_phase, vector_s, vector_c);
    out_buf[tid.x] = s + c + vector_s.x + vector_c.y;
}
