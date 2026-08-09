// Function-like macro arguments must be substituted simultaneously.
#define INVARIANT_MUL(Lhs, Rhs) ((Lhs) * (Rhs))
RWStructuredBuffer<float> out_buf : register(u0);

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float Rhs = float(tid.x) + 2.0;
    float Th = 3.0;
    // Sequential substitution must not rewrite Rhs inside the Lhs argument.
    out_buf[tid.x] = INVARIANT_MUL(-Rhs, Th);
}
