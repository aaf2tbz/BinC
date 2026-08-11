#define SCREEN_PASS_STRUCT_MEMBER(StructName, MemberType, MemberName) MemberType StructName##_##MemberName;
#define SCREEN_PASS_TEXTURE_VIEWPORT(StructName) \
    SCREEN_PASS_STRUCT_MEMBER(StructName, float2, Extent) \
    SCREEN_PASS_STRUCT_MEMBER(StructName, float2, ExtentInverse)
SCREEN_PASS_TEXTURE_VIEWPORT(AOViewport)
[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID) { }
