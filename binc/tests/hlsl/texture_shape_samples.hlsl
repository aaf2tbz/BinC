// UE helper ABIs: preserve 1D, 3D, and 2D-array texture shapes in AIR.
Texture1D<float4> OneD : register(t0);
Texture3D<float4> ThreeD : register(t1);
Texture2DArray<float4> Array2D : register(t2);
SamplerState LinearSampler : register(s0);
RWStructuredBuffer<float4> Output : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    Output[0] = OneD.SampleLevel(LinearSampler, 0.5, 0.0)
              + ThreeD.SampleLevel(LinearSampler, float3(0.5, 0.5, 0.5), 0.0)
              + Array2D.SampleLevel(LinearSampler, float3(0.5, 0.5, 0.0), 0.0);
}
