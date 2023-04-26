#include "Common.hlsli"
#include "Random.hlsli"
#include "Lighting.hlsli"
#include "VisibilityBuffer.hlsli"
#include "RayTracing/DDGICommon.hlsli"
#include "Noise.hlsli"

struct PerViewData
{
	uint4 ClusterDimensions;
	uint2 ClusterSize;
	float2 LightGridParams;
};
ConstantBuffer<PerViewData> cPass : register(b0);

Texture2D<uint> tVisibilityTexture : register(t0);
Texture2D<float> tAO :	register(t1);
Texture2D<float> tDepth : register(t2);
Texture2D tPreviousSceneColor :	register(t3);
Texture3D<float4> tFog : register(t4);
StructuredBuffer<MeshletCandidate> tVisibleMeshlets : register(t5);
StructuredBuffer<uint> tLightGrid : register(t6);
StructuredBuffer<uint> tLightIndexList : register(t7);

RWTexture2D<float4> uColorTarget : register(u0);
RWTexture2D<float2> uNormalsTarget : register(u1);
RWTexture2D<float> uRoughnessTarget : register(u2);

MaterialProperties EvaluateMaterial(MaterialData material, VisBufferVertexAttribute attributes)
{
	MaterialProperties properties;
	float4 baseColor = material.BaseColorFactor * Unpack_RGBA8_UNORM(attributes.Color);
	if(material.Diffuse != INVALID_HANDLE)
	{
		baseColor *= SampleGrad2D(NonUniformResourceIndex(material.Diffuse), sMaterialSampler, attributes.UV, attributes.DX, attributes.DY);
	}
	properties.BaseColor = baseColor.rgb;
	properties.Opacity = baseColor.a;

	properties.Metalness = material.MetalnessFactor;
	properties.Roughness = material.RoughnessFactor;
	if(material.RoughnessMetalness != INVALID_HANDLE)
	{
		float4 roughnessMetalnessSample = SampleGrad2D(NonUniformResourceIndex(material.RoughnessMetalness), sMaterialSampler, attributes.UV, attributes.DX, attributes.DY);
		properties.Metalness *= roughnessMetalnessSample.b;
		properties.Roughness *= roughnessMetalnessSample.g;
	}
	properties.Emissive = material.EmissiveFactor.rgb;
	if(material.Emissive != INVALID_HANDLE)
	{
		properties.Emissive *= SampleGrad2D(NonUniformResourceIndex(material.Emissive), sMaterialSampler, attributes.UV, attributes.DX, attributes.DY).rgb;
	}
	properties.Specular = 0.5f;

	properties.Normal = attributes.Normal;
	if(material.Normal != INVALID_HANDLE)
	{
		float3 normalTS = SampleGrad2D(NonUniformResourceIndex(material.Normal), sMaterialSampler, attributes.UV, attributes.DX, attributes.DY).rgb;
		float3x3 TBN = CreateTangentToWorld(properties.Normal, float4(normalize(attributes.Tangent.xyz), attributes.Tangent.w));
		properties.Normal = TangentSpaceNormalMapping(normalTS, TBN);
	}
	return properties;
}

uint GetSliceFromDepth(float depth)
{
	return floor(log(depth) * cPass.LightGridParams.x - cPass.LightGridParams.y);
}

void GetLightCount(float2 pixel, float linearDepth, out uint lightCount, out uint startOffset)
{
	uint3 clusterIndex3D = uint3(floor(pixel / cPass.ClusterSize), GetSliceFromDepth(linearDepth));
	uint tileIndex = Flatten3D(clusterIndex3D, cPass.ClusterDimensions.xyz);
	startOffset = tLightGrid[tileIndex * 2];
	lightCount = tLightGrid[tileIndex * 2 + 1];
}

Light GetLight(uint lightIndex, uint lightOffset)
{
	lightIndex = tLightIndexList[lightOffset + lightIndex];
	return GetLight(lightIndex);
}

LightResult DoLight(float3 specularColor, float R, float3 diffuseColor, float3 N, float3 V, float3 worldPos, float2 pixel, float linearDepth, float dither)
{
	LightResult totalResult = (LightResult)0;

	uint lightCount, lightOffset;
	GetLightCount(pixel, linearDepth, lightCount, lightOffset);

	for(uint i = 0; i < lightCount; ++i)
	{
		Light light = GetLight(i, lightOffset);
		LightResult result = DoLight(light, specularColor, diffuseColor, R, N, V, worldPos, linearDepth, dither);

		totalResult.Diffuse += result.Diffuse;
		totalResult.Specular += result.Specular;
	}

	return totalResult;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 texel = dispatchThreadId.xy;
	if(any(texel >= cView.TargetDimensions))
		return;
	float2 screenUV = ((float2)texel.xy + 0.5f) * cView.TargetDimensionsInv;
	float ambientOcclusion = tAO.SampleLevel(sLinearClamp, screenUV, 0);
	float linearDepth = LinearizeDepth(tDepth.SampleLevel(sPointClamp, screenUV, 0));
	float dither = InterleavedGradientNoise(texel.xy);

	uint candidateIndex, primitiveID;
	if(!UnpackVisBuffer(tVisibilityTexture[texel], candidateIndex, primitiveID))
		return;

	MeshletCandidate candidate = tVisibleMeshlets[candidateIndex];
    InstanceData instance = GetInstance(candidate.InstanceID);

	// Vertex Shader
	VisBufferVertexAttribute vertex = GetVertexAttributes(screenUV, instance, candidate.MeshletIndex, primitiveID);

	// Surface Shader
	MaterialData material = GetMaterial(instance.MaterialIndex);
	MaterialProperties surface = EvaluateMaterial(material, vertex);
	BrdfData brdfData = GetBrdfData(surface);

	float3 V = normalize(cView.ViewLocation - vertex.Position);
	float ssrWeight = 0;
	float3 ssr = ScreenSpaceReflections(vertex.Position, surface.Normal, V, brdfData.Roughness, tDepth, tPreviousSceneColor, dither, ssrWeight);

	LightResult result = DoLight(brdfData.Specular, brdfData.Roughness, brdfData.Diffuse, surface.Normal, V, vertex.Position, texel.xy, linearDepth, dither);

	float3 outRadiance = 0;
	outRadiance += ambientOcclusion * Diffuse_Lambert(brdfData.Diffuse) * SampleDDGIIrradiance(vertex.Position, surface.Normal, -V);
	outRadiance += result.Diffuse + result.Specular;
	outRadiance += ssr;
	outRadiance += surface.Emissive;

	float fogSlice = sqrt((linearDepth - cView.FarZ) / (cView.NearZ - cView.FarZ));
	float4 scatteringTransmittance = tFog.SampleLevel(sLinearClamp, float3(screenUV, fogSlice), 0);
	outRadiance = outRadiance * scatteringTransmittance.w + scatteringTransmittance.rgb;

	float reflectivity = saturate(Square(1 - brdfData.Roughness));

	uColorTarget[texel] = float4(outRadiance, surface.Opacity);
	uNormalsTarget[texel] = EncodeNormalOctahedron(surface.Normal);
	uRoughnessTarget[texel] = reflectivity;
}
