cbuffer ViewConstants : register(b0)
{
    float4x4 ViewToClip;
};

RWStructuredBuffer<float4> Output : register(u0);

bool IsOrthoProjection(float4x4 M)
{
    return M[3][3] == 0.0;
}

bool IsOrthoProjection()
{
    return IsOrthoProjection(ViewToClip);
}

bool select_internal(bool c, bool a, bool b)
{
    return c ? a : b;
}

float3 select_internal(bool c, float3 a, float3 b)
{
    return c ? a : b;
}

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    bool selected = select_internal(IsOrthoProjection(), true, false);
    float3 selected_vec = select_internal(false, float3(1.0, 2.0, 3.0), mul(float4(1.0, 2.0, 3.0, 1.0), ViewToClip).xyz);
    Output[0] = float4(selected_vec, selected ? 1.0 : 0.0);
}
