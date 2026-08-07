// Phase-7 differential test: fp16 `half` lowering.
// Strategy: half STORES round to fp16 (fptrunc), compute happens in f32.
// Every value here is fp16-exact, so ours matches the DXC default reference
// (which lowers half -> float32), while exercising half params, returns,
// struct fields, constructors, swizzles, and scalar/vector arithmetic.
RWStructuredBuffer<float4> out_buf : register(u0);

struct H { half2 uv; half s; };

half4 scale(half4 c, half w) {
    half4 r = c * w;
    H h;
    h.uv = r.xy;
    h.s = r.x + r.y;
    r.xy = h.uv;
    r.z = h.s;
    r.w = saturate(r.x);
    return r;
}

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    half t = half(tid.x % 8) * 0.5;
    half4 c = half4(1.5, 2.0, 0.5, 0.25) + half4(t, t, 0.0, 0.0);
    half3 v = half3(c.x, c.y, c.z) * half3(2.0, 1.5, 0.5);
    half s = v.x + v.y + v.z;
    half4 r = scale(c, 2.0) + half4(s, s, s, 1.0);
    out_buf[tid.x] = float4(r.x, r.y, r.z, r.w);
}
