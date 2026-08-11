struct FGPUSkinIndexAndWeight
{
    int4 BlendIndices;
    int4 BlendIndices2;
    float4 BlendWeights;
    float4 BlendWeights2;
};

struct FComputeBoneMatrixInputs
{
    Buffer<float4> BoneMatrices;
    Buffer<uint> SectionBoneMap;
    uint SectionBoneOffset;
    uint BoneTransformOffset;
    uint TransformStorageMode;
};

Buffer<float4> BoneMatrices;

float4 LoadCompressedBoneTransformTransposed(Buffer<float4> SrcBuffer, uint BaseOffset, uint BoneIndex)
{
    return SrcBuffer[BaseOffset + BoneIndex];
}

FComputeBoneMatrixInputs GetComputeBoneMatrixInputs(Buffer<float4> InBoneMatrices)
{
    FComputeBoneMatrixInputs Inputs;
    Inputs.BoneMatrices = InBoneMatrices;
    Inputs.SectionBoneOffset = 0;
    Inputs.BoneTransformOffset = 0;
    Inputs.TransformStorageMode = 0;
    return Inputs;
}

float4 LoadSkinningBoneTransform(FComputeBoneMatrixInputs Inputs, uint TransformIndex)
{
    uint TransformBufferIndex = Inputs.BoneTransformOffset + Inputs.SectionBoneOffset + TransformIndex;
    return LoadCompressedBoneTransformTransposed(Inputs.BoneMatrices, 0, TransformBufferIndex);
}

float4 ComputeBoneMatrixWithLimitedInfluences(FComputeBoneMatrixInputs Inputs, FGPUSkinIndexAndWeight In, bool bExtraInfluences = 0)
{
    return LoadSkinningBoneTransform(Inputs, In.BlendIndices.x);
}

float4 ComputeBoneMatrixWithLimitedInfluences2(Buffer<float4> InBoneMatrices, FGPUSkinIndexAndWeight In, bool bExtraInfluences = 0)
{
    FComputeBoneMatrixInputs Inputs = GetComputeBoneMatrixInputs(InBoneMatrices);
    return ComputeBoneMatrixWithLimitedInfluences(Inputs, In, bExtraInfluences);
}

RWStructuredBuffer<float4> Out;

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    FGPUSkinIndexAndWeight In = (FGPUSkinIndexAndWeight)0;
    Out[0] = ComputeBoneMatrixWithLimitedInfluences2(BoneMatrices, In, false);
}
