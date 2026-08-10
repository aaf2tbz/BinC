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
    float3 first_row = m[0];
    m[0] = first_row;
    float4x3 mats[2], mat_single;
    mats[0] = m;
    mat_single = m;
    float3 array_row = mats[0][0];
    float3 comma_row = mat_single[0];
    mats[0][0] = array_row;
    float array_scalar = mats[0][0][1];
    MatrixHolder h;
    h.M = m;
    Output[0] = t[0].x + v.x;
    Output[1] = h.M[0].x;
    Output[2] = first_row.y;
    Output[3] = array_row.z + array_scalar;
    Output[4] = comma_row.x;
}
