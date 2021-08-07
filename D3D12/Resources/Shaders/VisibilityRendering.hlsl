#include "Common.hlsli"
#include "CommonBindings.hlsli"
#include "Random.hlsli"

#define RootSig \
                "RootConstants(num32BitConstants=2, b0), " \
                "CBV(b1, visibility=SHADER_VISIBILITY_ALL), " \
                "DescriptorTable(SRV(t10, numDescriptors = 11)), " \
                GLOBAL_BINDLESS_TABLE ", " \
                "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, visibility = SHADER_VISIBILITY_PIXEL), "

struct PerViewData
{
    float4x4 ViewProjection;
};

ConstantBuffer<PerObjectData> cObjectData : register(b0);
ConstantBuffer<PerViewData> cViewData : register(b1);

struct Vertex
{
    uint2 Position;
    uint UV;
    float3 Normal;
    float4 Tangent;
};

[RootSignature(RootSig)]
float4 VSMain(uint vertexId : SV_VertexID) : SV_POSITION
{
    MeshInstance instance = tMeshInstances[cObjectData.Index];
    MeshData mesh = tMeshes[instance.Mesh];

    float3 position = UnpackHalf3(GetVertexData<uint2>(mesh.PositionStream, vertexId));
    return mul(mul(float4(position, 1.0f), instance.World), cViewData.ViewProjection);
}

void PSMain(
    float4 position : SV_POSITION,
    uint primitiveIndex : SV_PrimitiveID,
    float3 barycentrics : SV_Barycentrics,
    out uint outPrimitiveMask : SV_TARGET0,
    out float2 ouBarycentrics : SV_TARGET1)
{
    outPrimitiveMask = (cObjectData.Index << 16) | (primitiveIndex & 0xFFFF);
    ouBarycentrics = barycentrics.xy;
}