// Phase-3 MRT test: a fragment writing two color targets.
// Verified differentially via render-diff (nrt=2, dumppix on both targets).
struct PSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PSInput VSMain(float4 position : POSITION, float4 color : COLOR) {
    PSInput result;
    result.position = position;
    result.color = color;
    return result;
}

struct MRT {
    float4 base : SV_Target0;
    float4 key : SV_Target1;
};

MRT PSMain(PSInput input) : SV_Target0 {
    MRT result;
    result.base = input.color;
    result.key = float4(input.color.g, input.color.b, input.color.r, 1.0);
    return result;
}
