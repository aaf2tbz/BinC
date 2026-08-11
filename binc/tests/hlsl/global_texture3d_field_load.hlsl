struct GlobalTexture3DData
{
    Texture3D Tex;
};
GlobalTexture3DData U;
RWStructuredBuffer<float> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int4 coord = int4(0, 0, 0, 0);
    float4 value = U.Tex.Load(coord);
    Out[0] = value.x;
}
