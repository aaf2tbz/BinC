// D3D12-family regression file (each pattern fixed a Phase-1 bucket):
//  - ByteAddressBuffer.Load / Load2 / Store / Store2 (byte-offset addressing)
//  - RWStructuredBuffer<uint> stays atomic-capable (not a byte-address buffer)
//  - cbuffer array fields keep their element vector type (float3 arr[4].xy)
//  - mutable static globals (SPIRV-Cross `static uint3 gl_GlobalInvocationID`)
//  - SPIRV-Cross input struct (main(SPIRV_Cross_Input s) with an SV field)
//  - helper functions referencing module resources directly (arg capture)

struct SPIRV_Cross_Input {
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

cbuffer Constants : register(b0) {
    uint NumberOfElements;
    float3 Planes[4];
};

RWByteAddressBuffer g_SortBuffer : register(u0);
RWByteAddressBuffer g_Out : register(u1);
RWStructuredBuffer<uint> g_Atomic : register(u2);

static uint3 gl_GlobalInvocationID;

void LoadKeyIndexPair(uint Element)
{
    uint keyValue = g_SortBuffer.Load(Element * 4);
    g_SortBuffer.Store(Element * 4, keyValue + 1);
    InterlockedAdd(g_Atomic[Element], 1u);
}

[numthreads(8, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    LoadKeyIndexPair(gl_GlobalInvocationID.x);
    uint2 v = g_SortBuffer.Load2(gl_GlobalInvocationID.x * 16 + 8);
    g_Out.Store2(gl_GlobalInvocationID.x * 16 + 8, v);
    float2 p = Constants.Planes[0u].xy;
    if (gl_GlobalInvocationID.x < Constants.NumberOfElements)
        g_Out.Store(gl_GlobalInvocationID.x * 4, asuint(p.x));
}
