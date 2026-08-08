// BasePassFeatureFixture.hlsl — reduced regression fixture for the UE
// base-pass feature set that BasePassPixelShader.usf exercises:
//   * packed cbuffer vectors ([3 x float] fields, D3D register alignment)
//   * LWC tile-offset structs (FDFVector3 / FDFMatrix, MakeDFMatrix4x3)
//   * matrix by-value arguments and struct-field matrix stores
//   * CondMask-style int -> bool argument conversion
//   * defaulted function parameters
//   * clip() -> air.discard_fragment behind a branch
//   * SV_IsFrontFace -> air.front_facing (no user(locnN))
//   * an empty stage-in struct (FStereoVSToPS) contributing no inputs
cbuffer Primitive
{
    float4x4 LocalToWorld;
    float3 ObjectBoundsMin;
    float3 ObjectBoundsMax;
    float MinDisplacement;
    uint Flags;
};

cbuffer View
{
    float4x4 ViewToClip;
    float3 PreViewTranslation;
    float3 ViewForward;
};

struct FStereoVSToPS
{
};

struct FStereoPSInput
{
    FStereoVSToPS StereoInterpolants;
};

struct FDFVector3
{
    float3 High;
    float3 Low;
};

struct FDFMatrix
{
    float4x4 M;
    float3 PostTranslation;
};

struct FOut
{
    float4 color : SV_Target0;
    float4 extra : SV_Target1;
};

FDFVector3 MakeDFVector3(float3 High, float3 Low)
{
    FDFVector3 r;
    r.High = High;
    r.Low = Low;
    return r;
}

FDFVector3 WSSubtract(FDFVector3 A, FDFVector3 B)
{
    FDFVector3 r;
    r.High = A.High - B.High;
    r.Low = A.Low - B.Low;
    return r;
}

float3 DFToTileOffset(FDFVector3 In)
{
    return In.High;
}

float4x4 Make4x3Matrix(float3 PostTranslation)
{
    float4x4 m = float4x4(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    m[0][0] = 1.0; m[1][1] = 1.0; m[2][2] = 1.0; m[3][3] = 1.0;
    m[3].xyz = PostTranslation;
    return m;
}

FDFMatrix MakeDFMatrix4x3(float3 PostTranslation, float4x4 InMatrix)
{
    FDFMatrix r;
    r.M = InMatrix;
    r.PostTranslation = PostTranslation;
    return r;
}

float CondMask(bool Mask, float A, float B)
{
    return Mask ? A : B;
}

void FPixelShaderInOut_MainPS(in float4 In, out float4 Out, float Opt = 1.0, float Opt2 = 2.0)
{
    Out = In * Opt + Opt2;
}

FOut main(in float4 SvPosition : SV_Position, in bool bIsFrontFace : SV_IsFrontFace, FStereoPSInput StereoInput : STEREO)
{
    FOut o;
    float3 p = Primitive.ObjectBoundsMin + View.PreViewTranslation;
    FDFVector3 a = MakeDFVector3(p, 0);
    FDFVector3 b = WSSubtract(a, a);
    float3 t = DFToTileOffset(b);
    float4x4 m = Make4x3Matrix(t);
    FDFMatrix dm = MakeDFMatrix4x3(t, Primitive.LocalToWorld);
    float mask = CondMask(Flags & 1u, 1.0, 0.0);
    float4 out4 = (float4)0;
    FPixelShaderInOut_MainPS(SvPosition, out4, mask);
    clip(SvPosition.w - 0.5);
    o.color = float4(t * mask + out4.xyz, bIsFrontFace ? 1.0 : 0.0);
    o.extra = float4(m[3].xyz, dm.PostTranslation.x);
    return o;
}

struct FVSOut
{
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
};

FVSOut mainVS(float4 pos : POSITION)
{
    FVSOut o;
    o.pos = mul(View.ViewToClip, pos) + Primitive.ObjectBoundsMin.x;
    o.world = pos.xyz;
    return o;
}
