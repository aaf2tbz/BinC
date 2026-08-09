// A swizzle read from a const scalar is a value splat, never a write.
static const float MaxValue = 6.5;
RWStructuredBuffer<float> out_buf : register(u0);
[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float2 pair = MaxValue.xx;
    out_buf[tid.x] = pair.x + pair.y + MaxValue.x;
}
