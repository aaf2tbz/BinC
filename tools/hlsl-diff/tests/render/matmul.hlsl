// Phase-6 matrix conventions: HLSL row-major cbuffer matrices + mul()
// (m1*m2 and row-vector*m), ours vs the DXC->SPIRV-Cross reference.
cbuffer C : register(b0) {
    float4x4 World;      // identity
    float4x4 ViewProj;   // 0.5 scale + z translation
};

struct PSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PSInput VSMain(float4 position : POSITION, float4 color : COLOR) {
    PSInput result;
    float4x4 mvp = mul(World, ViewProj);
    result.position = mul(position, mvp);   // row-vector * matrix
    result.color = color;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return input.color;
}
