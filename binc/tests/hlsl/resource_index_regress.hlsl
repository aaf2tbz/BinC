Texture2D<float> Src;
RWTexture2D<float> Dst;

[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 p = uint2(0,0);
    float value = Src[p].x;
    Dst[p] = value;
}
