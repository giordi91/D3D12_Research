#ifdef __cplusplus
#pragma once
namespace ShaderInterop
{
#endif

#define MAX_SHADOW_CASTERS 32

#ifdef __cplusplus
	using float2 = Vector2;
	using float3 = Vector3;
	using float4 = Vector4;
	using uint = uint32;
	using uint2 = TIntVector2<uint32>;
	using uint3 = TIntVector3<uint32>;
	using uint4 = TIntVector4<uint32>;
	using int2 = TIntVector2<int32>;
	using int3 = TIntVector3<int32>;
	using int4 = TIntVector4<int32>;
	using float4x4 = Matrix;

	template<typename T> struct ConstantBuffer { T Data; };
#endif

	static const int MESHLET_MAX_TRIANGLES = 124;
	static const int MESHLET_MAX_VERTICES = 64;

	// Per material shader data
	struct MaterialData
	{
		uint Diffuse;
		uint Normal;
		uint RoughnessMetalness;
		uint Emissive;
		float4 BaseColorFactor;
		float4 EmissiveFactor;
		float MetalnessFactor;
		float RoughnessFactor;
		float AlphaCutoff;
	};

	// The normals vertex stream data
	struct NormalData
	{
		float3 Normal;
		float4 Tangent;
	};

	struct MeshData
	{
		uint BufferIndex;
		uint PositionsOffset;
		uint UVsOffset;
		uint NormalsOffset;
		uint ColorsOffset;
		uint IndicesOffset;
		uint IndexByteSize;

		uint MeshletOffset;
		uint MeshletVertexOffset;
		uint MeshletTriangleOffset;
		uint MeshletBoundsOffset;
		uint MeshletCount;
	};

	struct MeshletTriangle
	{
		uint V0 : 10;
		uint V1 : 10;
		uint V2 : 10;
		uint padding : 2;
	};

	struct Meshlet
	{
		uint VertexOffset;
		uint TriangleOffset;
		uint VertexCount;
		uint TriangleCount;
	};

	struct MeshletBounds
	{
		float3 Center;
		float Radius;
		//float3 ConeApex;
		float3 ConeAxis;
		float ConeCutoff;
		//uint ConeS8;
	};

	struct MeshInstance
	{
		uint Material;
		uint Mesh;
		uint World;
	};

	typedef MeshInstance InstanceData;

	inline float4 UIntToColor(uint c)
	{
		return float4(
			(float)(((c >> 24) & 0xFF) / 255.0f),
			(float)(((c >> 16) & 0xFF) / 255.0f),
			(float)(((c >> 8) & 0xFF) / 255.0f),
			(float)(((c >> 0) & 0xFF) / 255.0f)
		);
	}

	inline bool EnumHasAnyFlag(uint value, uint mask)
	{
		return (value & mask) != 0;
	}

	inline bool EnumHasAllFlags(uint value, uint mask)
	{
		return (value & mask) == mask;
	}

	struct Light
	{
		float3 Position;
		uint Color;
		float3 Direction;
		float Intensity;
		float2 SpotlightAngles;
		float Range;
		float InvShadowSize;

		uint ShadowMapIndex;
		uint MatrixIndex;
		uint MaskTexture;

		// flags
		uint IsEnabled : 1;
		uint IsSpot : 1;
		uint IsPoint: 1;
		uint IsDirectional : 1;
		uint IsVolumetric : 1;
		uint CastShadows : 1;

#ifndef __cplusplus
		float3 GetColor() { return UIntToColor(Color).rgb; }
#endif

		bool PointAttenuation() { return IsPoint || IsSpot; }
		bool DirectionalAttenuation() { return IsSpot; }
	};

	struct DDGIVolume
	{
		float3 BoundsMin;
		uint DepthIndex;
		float3 ProbeSize;
		uint IrradianceIndex;
		uint3 ProbeVolumeDimensions;
		uint ProbeOffsetIndex;
		uint ProbeStatesIndex;
		uint NumRaysPerProbe;
		uint MaxRaysPerProbe;
	};

	struct ViewUniforms
	{
		float4x4 LightMatrices[MAX_SHADOW_CASTERS];
		float4 CascadeDepths;
		uint NumCascades;
		uint3 padd0;

		float4x4 View;
		float4x4 ViewInverse;
		float4x4 Projection;
		float4x4 ProjectionInverse;
		float4x4 ViewProjection;
		float4x4 ViewProjectionInverse;
		float4x4 PreviousViewProjection;
		float4x4 ReprojectionMatrix;
		float3 ViewLocation;
		float padd1;
		float4 FrustumPlanes[6];
		float2 TargetDimensions;
		float2 TargetDimensionsInv;
		float2 ViewportDimensions;
		float2 ViewportDimensionsInv;
		float2 ViewJitter;
		float NearZ;
		float FarZ;
		float FoV;

		uint FrameIndex;
		uint SsrSamples;
		uint LightCount;
		uint NumDDGIVolumes;

		uint TLASIndex;
		uint MeshesIndex;
		uint MaterialsIndex;
		uint MeshInstancesIndex;
		uint TransformsIndex;
		uint LightsIndex;
		uint SkyIndex;
		uint DDGIVolumesIndex;
	};

#ifdef __cplusplus
}
#endif