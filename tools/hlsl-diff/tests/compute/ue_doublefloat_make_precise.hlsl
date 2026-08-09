// Regression for UE DoubleFloat's parenthesized MakePrecise expressions.
struct FDFScalar { float High; float Low; };
FDFScalar MakeDFScalar(float High, float Low) { FDFScalar result; result.High = High; result.Low = Low; return result; }
precise float MakePrecise(in precise float v) { precise float pv = v; return pv; }

FDFScalar DFTwoSum(float Lhs, float Rhs) {
    const float S = MakePrecise((Lhs) + (Rhs));
    const float V = MakePrecise((S) - (Lhs));
    const float Q = MakePrecise((S) - (V));
    const float R = MakePrecise((Lhs) - (Q));
    const float T = MakePrecise((V) - (V));
    const float Y = MakePrecise((R) + (T));
    return MakeDFScalar(S, Y);
}

RWStructuredBuffer<float> out_buf : register(u0);
[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    FDFScalar value = DFTwoSum(float(tid.x), 0.5);
    out_buf[tid.x] = value.High + value.Low;
}
