// Phase-6 matrix compute differential: constructors, mul() overloads
// (m1*m2, v*m, v1*v2 dot), matrix by-value params and returns.
RWStructuredBuffer<float> out_buf : register(u0);

float4x4 scale(float s) {
    return float4x4(s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1);
}

float4x4 rot(float a) {
    float c = cos(a);
    float s = sin(a);
    return float4x4(c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
}

[numthreads(4, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    float4x4 m = mul(scale(2.0), rot(0.5));     // m1*m2
    float4 v = mul(float4(1, 2, 3, 4), m);      // row-vector * matrix
    float d = dot(float3(1, 2, 3), float3(4, 5, 6));  // v1*v2
    out_buf[DTid.x] = v.x * 1000 + v.y * 100 + v.z * 10 + v.w + d * 0.001;
}
