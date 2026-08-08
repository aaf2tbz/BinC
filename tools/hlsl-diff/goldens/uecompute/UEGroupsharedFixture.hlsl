// UEGroupsharedFixture.hlsl — reduced UE-compute fixture: groupshared memory
// with the threadgroup barrier, LWC tile-offset structs, and a dispatch-id
// coordinate, in the binc compute pipeline (the ue-audit CS pattern).
cbuffer Params
{
    float3 Center;
    uint Count;
};

groupshared float4 gsBuf[64];

struct FDFVector3
{
    float3 High;
    float3 Low;
};

FDFVector3 MakeDFVector3(float3 High, float3 Low)
{
    FDFVector3 r;
    r.High = High;
    r.Low = Low;
    return r;
}

float3 DFToTileOffset(FDFVector3 In)
{
    return In.High;
}

RWStructuredBuffer<float4> Out : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    gsBuf[id.x] = float4(DFToTileOffset(MakeDFVector3(Center, 0)) + id.x, 1.0);
    sync();
    float4 v = gsBuf[Count & 63u];
    Out[id.x] = v + float4(0.5, 0.25, 0.125, 0.0);
}
