// Non-square HLSL matrices retain rows/columns through AIR lowering.
RWStructuredBuffer<float> Output : register(u0);
struct MatrixHolder { float4x3 M; };
float4x3 MakeMatrix() {
    return float4x3(float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0), float3(1.0, 1.0, 1.0));
}
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float4x3 m = MakeMatrix();
    float3x4 t = transpose(m);
    float3 v = mul(float4(1.0, 2.0, 3.0, 1.0), m);
    MatrixHolder h;
    h.M = m;
    Output[0] = t[0].x + v.x;
    Output[1] = h.M[0].x;
}
