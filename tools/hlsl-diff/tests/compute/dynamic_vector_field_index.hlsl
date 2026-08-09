// Dynamic indexing into a vector-valued struct field is a value extract, not a buffer pointer.
struct Pair { float2 High; float2 Low; };
RWStructuredBuffer<float> out_buf : register(u0);

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    Pair value;
    value.High = float2(float(tid.x), float(tid.x) + 10.0);
    value.Low = float2(1.0, 2.0);
    int component = int(tid.x & 1u);
    out_buf[tid.x] = value.High[component] + value.Low[component];
}
