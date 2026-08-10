struct ResourceView
{
    Texture2D Tex;
    SamplerState Samp;
};

ResourceView View;
RWStructuredBuffer<float4> Output : register(u0);

float4 Texture2DSampleLevel(Texture2D Tex, SamplerState Sampler, float2 UV, float Mip)
{
    return Tex.SampleLevel(Sampler, UV, Mip);
}

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Output[0] = Texture2DSampleLevel(View.Tex, View.Samp, float2(0.0, 0.0), 0.0);
}
