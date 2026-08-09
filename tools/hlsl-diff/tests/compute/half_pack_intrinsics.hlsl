// SM6 packing intrinsic regression: HLSL float32 <-> IEEE-754 binary16 bits.
// The differential runner compares scalar, float2, and float3 packing to DXC
// -> SPIRV-Cross -> Metal. Keep the source free of local aggregate initialization
// so a failure isolates the intrinsic rather than an unrelated array-lowering gap.
RWStructuredBuffer<float> out_buf : register(u0);

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float input = (float(tid.x & 7) - 4.0) * 0.5;
    uint packed_scalar = f32tof16(input);
    uint2 packed_vector2 = f32tof16(float2(input, -input));
    uint3 packed_vector3 = f32tof16(float3(input, -input, input * 0.5));
    float scalar_roundtrip = f16tof32(packed_scalar);
    float2 vector2_roundtrip = f16tof32(packed_vector2);
    float3 vector3_roundtrip = f16tof32(packed_vector3);
    out_buf[tid.x] = scalar_roundtrip + vector2_roundtrip.x + vector2_roundtrip.y
        + vector3_roundtrip.x + vector3_roundtrip.y + vector3_roundtrip.z;
}
