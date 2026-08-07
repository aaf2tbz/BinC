// Preprocessor regression file (UE-style constructs that each fixed a bug):
//  - `#if<TAB>` / `#define<TAB>` directives (tab after the word)
//  - `#ifndef` with a -D define (neg check was reading the wrong char)
//  - `#define/#undef` position-correct scoping (FDFType template pattern)
//  - function-like macro expansion introducing another macro's name
//  - bool2/3/4 vector types
//  - unsized array `float a[] = { ... }` locals
//  - C++-style `RetType Struct::Method(...)` out-of-line definitions
//  - `_Static_assert` and `typedef` at top level
//  - `precise` qualifiers on returns/params/locals
// Compile with: binc -E main -T cs_5_0 -D SM6_PROFILE tests/hlsl/pp_regress.hlsl

#define SM6_PROFILE_UNUSED 0

// `#if\t` with a TAB after the word (SceneTexturesCommon.ush pattern)
#if	SM6_PROFILE
#define A8_SAMPLE_MASK .r
#else
#define A8_SAMPLE_MASK .r
#endif

// `#ifndef` must see a -D define as defined (p[2]=='n' bug)
#ifndef SM6_PROFILE
#define SM6_PROFILE 0
#endif

// `#define\t` with a TAB after the word
#define	TAB_DEFINE 3

// position-correct #define/#undef scoping (FDFType instantiation pattern)
#define FDFType float2
#define DFConstructor MakeDFVector2
struct FDFVector2 { FDFType High; };
#undef FDFType
#undef DFConstructor

#define FDFType float3
#define DFConstructor MakeDFVector3
struct FDFVector3 { FDFType High; };
#undef FDFType
#undef DFConstructor

#define FDFType float4
#define DFConstructor MakeDFVector4
struct FDFVector4 { FDFType High; };
#undef FDFType
#undef DFConstructor

// function-like macro whose body names an object-like macro (multi-pass)
#define DEFINE_BROADCASTED_OP(Type, OP) \
    Type  OP##Demote(FDFScalar Lhs, FDFType Rhs) { return OP##Demote(Lhs, Rhs); }
#define FDFScalar float
#define FDFType FDFVector4
DEFINE_BROADCASTED_OP(FDFVector4, DFMultiply)
#undef FDFType

// bool vector types parse (bool2/3/4 type-parse regression; codegen of
// i1 vectors is Phase 3 scope)
// bool2 Mask2 = bool2(true, false);
// bool4 Mask4 = bool4(1, 0, 1, 0);

// C++-style out-of-line method + typedef + _Static_assert
typedef FDFVector4 RandomSequence;
_Static_assert(sizeof(FDFVector4) == 16, "size");

// precise qualifiers (lexed as const)
precise float MakePrecise(in precise float v) { precise float pv = v; return pv; }

RWStructuredBuffer<float> OutBuf : register(u0);

[numthreads(1,1,1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    // unsized array local with brace initializer (parse-level regression;
    // codegen indexes sized arrays only)
    float Alpha[] = { 0.5, 1.0, 1.5 };
    float Beta[3] = { 0.5, 1.0, 1.5 };
    // array indexing (sized)
    float C = Beta[0] + Beta[2];
    // matrix ctor (floatNxM dispatch — strlen guard bug)
    float3x3 M = float3x3(1.0);
    // bool vector types + constructor parse (bool2/3/4 type-parse regression)
    // bool2 B = bool2(true, true);
    // FDFVector2/3/4 all resolve to their own struct (not all FDFVector4)
    FDFVector2 V2 = MakeDFVector2(1.0, 2.0);
    FDFVector4 V4 = MakeDFVector4(1.0, 2.0, 3.0, 4.0);
    OutBuf[tid.x] = C + M[0][0] + V2.High.x + V4.High.w + MakePrecise(TAB_DEFINE);
}

FDFVector2 MakeDFVector2(float a, float b) { FDFVector2 r; r.High = float2(a, b); return r; }
FDFVector4 MakeDFVector4(float a, float b, float c, float d) { FDFVector4 r; r.High = float4(a, b, c, d); return r; }
