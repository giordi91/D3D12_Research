#include "stdafx.h"
#include "RTReflections.h"
#include "Graphics/Core/Shader.h"
#include "Graphics/Core/RootSignature.h"
#include "Graphics/Core/Graphics.h"
#include "Graphics/Core/CommandContext.h"
#include "Graphics/Core/Texture.h"
#include "Graphics/Core/ShaderBindingTable.h"
#include "Graphics/Core/ResourceViews.h"
#include "Graphics/Core/StateObject.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/SceneView.h"

RTReflections::RTReflections(GraphicsDevice* pDevice)
	: m_pDevice(pDevice)
{
	if (pDevice->GetCapabilities().SupportsRaytracing())
	{
		SetupPipelines(pDevice);
	}
}

void RTReflections::Execute(RGGraph& graph, const SceneView& sceneData, const SceneTextures& sceneTextures)
{
	RGPassBuilder rt = graph.AddPass("RT Reflections");
	rt.Bind([=](CommandContext& context, const RGPassResources& /*passResources*/)
		{
			Texture* pTarget = m_pSceneColor;

			context.CopyTexture(sceneTextures.pColorTarget, pTarget);

			context.InsertResourceBarrier(sceneTextures.pDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			context.InsertResourceBarrier(sceneTextures.pNormalsTarget, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			context.InsertResourceBarrier(sceneTextures.pRoughnessTarget, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			context.InsertResourceBarrier(sceneTextures.pColorTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			context.InsertResourceBarrier(m_pSceneColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			context.SetComputeRootSignature(m_pGlobalRS);
			context.SetPipelineState(m_pRtSO);

			struct Parameters
			{
				float ViewPixelSpreadAngle;
			} parameters{};

			parameters.ViewPixelSpreadAngle = atanf(2.0f * tanf(sceneData.View.FoV / 2) / (float)pTarget->GetHeight());

			ShaderBindingTable bindingTable(m_pRtSO);
			bindingTable.BindRayGenShader("RayGen");
			bindingTable.BindMissShader("ReflectionMiss", 0);
			bindingTable.BindMissShader("ShadowMiss", 1);
			bindingTable.BindHitGroup("ReflectionHitGroup", 0);

			const D3D12_CPU_DESCRIPTOR_HANDLE srvs[] = {
				sceneTextures.pDepth->GetSRV()->GetDescriptor(),
				pTarget->GetSRV()->GetDescriptor(),
				sceneTextures.pNormalsTarget->GetSRV()->GetDescriptor(),
				sceneTextures.pRoughnessTarget->GetSRV()->GetDescriptor(),
			};

			context.SetRootConstants(0, parameters);
			context.SetRootCBV(1, GetViewUniforms(sceneData, sceneTextures.pColorTarget));
			context.BindResource(2, 0, sceneTextures.pColorTarget->GetUAV());
			context.BindResources(3, 0, srvs, ARRAYSIZE(srvs));

			context.DispatchRays(bindingTable, sceneTextures.pColorTarget->GetWidth(), sceneTextures.pColorTarget->GetHeight());
		});
}

void RTReflections::OnResize(uint32 width, uint32 height)
{
	m_pSceneColor = m_pDevice->CreateTexture(TextureDesc::Create2D(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, TextureFlag::ShaderResource, 1, 1), "SceneColor Copy");
}

void RTReflections::SetupPipelines(GraphicsDevice* pDevice)
{
	m_pGlobalRS = new RootSignature(pDevice);
	m_pGlobalRS->AddRootConstants(0, 1);
	m_pGlobalRS->AddConstantBufferView(100);
	m_pGlobalRS->AddDescriptorTableSimple(0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8);
	m_pGlobalRS->AddDescriptorTableSimple(0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8);
	m_pGlobalRS->Finalize("Global");

	StateObjectInitializer stateDesc;
	stateDesc.Name = "RT Reflections";
	stateDesc.RayGenShader = "RayGen";
	stateDesc.AddLibrary(pDevice->GetLibrary("RTReflections.hlsl"));
	stateDesc.AddHitGroup("ReflectionHitGroup", "ReflectionClosestHit", "ReflectionAnyHit");
	stateDesc.AddMissShader("ReflectionMiss");
	stateDesc.AddMissShader("ShadowMiss");
	stateDesc.MaxPayloadSize = 5 * sizeof(float);
	stateDesc.MaxAttributeSize = 2 * sizeof(float);
	stateDesc.MaxRecursion = 2;
	stateDesc.pGlobalRootSignature = m_pGlobalRS;
	m_pRtSO = pDevice->CreateStateObject(stateDesc);
}
