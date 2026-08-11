struct ResourceBundle
{
    Buffer<float4> Data;
};

float ReadBundle(ResourceBundle Bundle)
{
    return Bundle.Data[0].x;
}

[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    ResourceBundle Bundle;
    float Value = ReadBundle(Bundle);
}
