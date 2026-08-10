// HLSL Gather* channel methods must preserve the selected component in AIR.
Texture2D<float4> Source : register(t0);
TextureCube<float4> CubeSource : register(t1);
Texture2DArray<float4> ArraySource : register(t2);
TextureCubeArray<float4> CubeArraySource : register(t3);
Texture2D<float> ScalarSource : register(t4);
SamplerState LinearSampler : register(s0);
RWStructuredBuffer<float4> Output : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float2 uv = float2(0.5, 0.5);
    Output[0] = Source.GatherRed(LinearSampler, uv);
    Output[1] = Source.GatherGreen(LinearSampler, uv);
    Output[2] = Source.GatherBlue(LinearSampler, uv);
    Output[3] = Source.GatherAlpha(LinearSampler, uv);
    uint status;
    Output[4] = CubeSource.GatherRed(LinearSampler, float3(0.0, 0.0, 1.0), status);
    Output[5] = ArraySource.GatherGreen(LinearSampler, float3(0.5, 0.5, 0.0));
    Output[6] = CubeArraySource.GatherBlue(LinearSampler, float4(0.0, 0.0, 1.0, 0.0), status);
    Output[7] = ScalarSource.Gather(LinearSampler, uv);
}
