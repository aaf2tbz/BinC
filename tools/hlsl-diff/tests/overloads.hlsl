// Phase-2 differential test: overload resolution.
// Ranked selection: exact > int->float promotion > scalar->vector splat.
RWStructuredBuffer<float> out_buf : register(u0);

float pick(float a, float b)          { return a + b; }
float pick(float2 a, float2 b)        { return dot(a, b); }
float pick(float a, float b, float c) { return a * b * c; }
float picki(int a, int b)             { return float(a + b); }

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float x = float(tid.x) - 8.0;
    float r = 0.0;
    r += pick(x, 1.0);                          // exact, 2-arg float
    r += pick(float2(x, 1.0), float2(2.0, 3.0)); // exact, float2 overload
    r += pick(1, 2);                            // int -> float promotion
    r += pick(x, 1.0, 2.0);                     // exact, 3-arg float
    r += pick(1.0, float2(2.0, 3.0));           // scalar splat into float2 overload
    r += picki(tid.x, 3);                       // exact int overload
    out_buf[tid.x] = r;
}
