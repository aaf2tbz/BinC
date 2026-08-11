struct GlobalTextureData
{
    Texture2D Tex;
};
GlobalTextureData U;
RWStructuredBuffer<float> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int3 coord = int3(0, 0, 0);
    float4 value = U.Tex.Load(coord);
    Out[0] = value.x;
}
