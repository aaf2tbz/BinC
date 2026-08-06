// Phase-2 differential test: math intrinsics.
// out[tid] accumulates one term per intrinsic family over a range of inputs.
RWStructuredBuffer<float> out_buf : register(u0);

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float x = float(tid.x) - 8.0;          // -8 .. 7
    float a = x * 0.1;
    float3 p = float3(a, a + 1.0, a - 1.0);
    float3 q = float3(a * 2.0, -a, a + 0.5);
    float r = 0.0;
    r += saturate(a);
    r += lerp(1.0, 3.0, a * 0.5 + 0.5);
    r += clamp(a, -0.5, 0.5);
    r += dot(p, q);
    r += length(p);
    r += distance(p, q);
    r += length(cross(p, q));
    r += length(normalize(p));
    r += length(reflect(p, q));
    r += abs(a);
    r += min(a, 0.25);
    r += max(a, -0.25);
    r += fmod(a, 0.7);
    r += frac(a);
    r += floor(a);
    r += ceil(a);
    r += step(0.0, a);
    r += smoothstep(-0.5, 0.5, a);
    r += radians(a);
    r += degrees(a);
    r += sin(a) + cos(a) + tan(a);
    r += asin(saturate(a)) + acos(saturate(a)) + atan(a);
    r += pow(abs(a) + 0.5, 1.5);
    r += exp(a) + exp2(a);
    r += log(abs(a) + 0.5) + log2(abs(a) + 0.5);
    r += sqrt(abs(a) + 0.5) + rsqrt(abs(a) + 0.5);
    r += mul(p, q);                        // mul(vec, vec) == dot
    out_buf[tid.x] = r;
}
