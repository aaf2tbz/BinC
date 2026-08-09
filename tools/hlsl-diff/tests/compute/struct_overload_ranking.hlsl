// Overload ranking must distinguish struct tags, not just T_STRUCT.
struct ScalarPair { float value; };
struct VectorPair { float value; };

ScalarPair Negate(ScalarPair value) { return value; }
VectorPair Negate(VectorPair value) { VectorPair result; result.value = -value.value; return result; }
VectorPair Add(VectorPair lhs, ScalarPair rhs) { return lhs; }
VectorPair Add(VectorPair lhs, VectorPair rhs) { VectorPair result; result.value = lhs.value + rhs.value; return result; }
VectorPair Subtract(VectorPair lhs, VectorPair rhs) { return Add(lhs, Negate(rhs)); }

RWStructuredBuffer<float> out_buf : register(u0);
[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    VectorPair lhs; lhs.value = float(tid.x) + 1.0;
    VectorPair rhs; rhs.value = 0.25;
    out_buf[tid.x] = Subtract(lhs, rhs).value;
}
