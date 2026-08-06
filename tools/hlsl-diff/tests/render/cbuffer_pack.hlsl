// Phase-4 cbuffer packing differential: tight D3D register packing
// (float + packed float2 + float4 crossing a register boundary).
cbuffer Tight : register(b0) {
    float a;      // register 0, bytes 0-4
    float2 b;     // register 0, bytes 4-12 (packed)
    float4 c;     // register 1, bytes 16-32
    float2 d;     // register 2, bytes 32-40
    float3 e;     // register 2, bytes 40-52
};

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

float4 PSMain(PSInput input) : SV_TARGET {
    // every member is read so a packing mismatch shifts the result
    return float4(a, b.x, b.y, c.x) + float4(d.x, d.y, e.x, e.y);
}
