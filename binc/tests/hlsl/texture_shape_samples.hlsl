// UE helper ABIs: preserve 1D, 3D, and 2D-array texture shapes in AIR.
Texture1D<float4> OneD : register(t0);
Texture3D<float4> ThreeD : register(t1);
Texture2DArray<float4> Array2D : register(t2);
TextureCube<float4> Cube : register(t3);
SamplerState LinearSampler : register(s0);
RWStructuredBuffer<float4> Output : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    Output[0] = OneD.Sample(LinearSampler, 0.5);
    Output[1] = OneD.SampleLevel(LinearSampler, 0.5, 1.0);
    Output[2] = ThreeD.Sample(LinearSampler, float3(0.5, 0.5, 0.5));
    Output[3] = ThreeD.SampleLevel(LinearSampler, float3(0.5, 0.5, 0.5), 1.0);
    Output[4] = Array2D.Sample(LinearSampler, float3(0.5, 0.5, 0.0));
    Output[5] = Array2D.SampleLevel(LinearSampler, float3(0.5, 0.5, 0.0), 1.0);
    Output[6] = Cube.Sample(LinearSampler, float3(0.0, 0.0, 1.0));
    Output[7] = Cube.SampleLevel(LinearSampler, float3(0.0, 0.0, 1.0), 1.0);
}
