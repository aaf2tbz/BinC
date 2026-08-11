Texture2D<float> Tex : register(t0);
RWStructuredBuffer<uint> Out : register(u0);

[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint3 Dims;
    Tex.GetDimensions(0, Dims.x, Dims.y, Dims.z);
    Out[0] = Dims.x + Dims.y + Dims.z;
}
