// Phase-2 differential test: bit reinterpretation (asfloat / asuint / asint).
// Bits round-trip through the as* family; the sums land back in float space.
RWStructuredBuffer<float> out_buf : register(u0);

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float x = float(tid.x) - 8.0;
    float4 v = float4(x, x * 2.0, -x, 1.0);
    // asuint -> asfloat must round-trip exactly (bits preserved both ways)
    out_buf[tid.x] = asfloat(asuint(x)) + asfloat(asint(x))
                   + asfloat(asuint(v)).x + asfloat(asuint(v)).y
                   + asfloat(asuint(v)).z + asfloat(asuint(v)).w;
}
