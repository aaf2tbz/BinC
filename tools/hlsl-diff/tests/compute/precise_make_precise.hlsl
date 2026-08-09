// UE Platform.ush exposes MakePrecise overloads with precise qualifiers.
precise float MakePrecise(in precise float v) { precise float pv = v; return pv; }
precise float2 MakePrecise(in precise float2 v) { precise float2 pv = v; return pv; }

float StableSum(float Lhs, float Rhs) {
    // Parenthesized operands must remain expressions, not unknown-type casts.
    const float S = MakePrecise((Lhs) + (Rhs));
    return S;
}

RWStructuredBuffer<float> out_buf : register(u0);
[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float input = float(tid.x) * 0.25;
    float scalar = StableSum(input, 0.5);
    float2 vector = MakePrecise(float2(input, -input));
    out_buf[tid.x] = scalar + vector.x + vector.y;
}
