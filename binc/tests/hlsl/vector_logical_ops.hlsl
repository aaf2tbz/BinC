// HLSL && and || are componentwise for bool vectors.
RWStructuredBuffer<float> out_buf : register(u0);
[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float2 value = float2(float(tid.x), float(tid.x) - 4.0);
    bool2 positive = value > 0.0;
    bool2 small = value < 8.0;
    bool2 selected = positive && small;
    bool2 outside = !positive || !small;
    out_buf[tid.x] = (selected.x ? 1.0 : 0.0) + (selected.y ? 2.0 : 0.0) +
                     (outside.x ? 4.0 : 0.0) + (outside.y ? 8.0 : 0.0);
}
