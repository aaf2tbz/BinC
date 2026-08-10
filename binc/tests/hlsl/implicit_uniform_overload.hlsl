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

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Output[0] = float4(IsOrthoProjection() ? 1.0 : 0.0, 0.0, 0.0, 0.0);
}
