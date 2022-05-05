#include "stdafx.h"
#include "GpuParticles.h"
#include "Graphics/RHI/Buffer.h"
#include "Graphics/RHI/Graphics.h"
#include "Graphics/RHI/PipelineState.h"
#include "Graphics/RHI/RootSignature.h"
#include "Graphics/RHI/CommandContext.h"
#include "Graphics/RHI/GraphicsResource.h"
#include "Graphics/RHI/Texture.h"
#include "Graphics/RHI/ResourceViews.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/SceneView.h"

static bool g_Enabled = true;
static int32 g_EmitCount = 30;
static float g_LifeTime = 4.0f;
static bool g_Simulate = true;

static constexpr uint32 cMaxParticleCount = 2000000;

struct ParticleData
{
	Vector3 Position;
	float LifeTime;
	Vector3 Velocity;
	float Size;
};

GpuParticles::GpuParticles(GraphicsDevice* pDevice)
	: m_pDevice(pDevice)
{
	CommandContext* pContext = pDevice->AllocateCommandContext(D3D12_COMMAND_LIST_TYPE_DIRECT);

	m_pCountersBuffer = pDevice->CreateBuffer(BufferDesc::CreateByteAddress(sizeof(uint32) * 4), "Particles Counter");
	BufferDesc particleBufferDesc = BufferDesc::CreateStructured(cMaxParticleCount, sizeof(uint32));
	m_pAliveList1 = pDevice->CreateBuffer(particleBufferDesc, "Particles Alive List 1");
	m_pAliveList2 = pDevice->CreateBuffer(particleBufferDesc, "Particles Alive List 2");
	m_pDeadList = pDevice->CreateBuffer(particleBufferDesc, "Particles Dead List");
	std::vector<uint32> deadList(cMaxParticleCount);
	std::generate(deadList.begin(), deadList.end(), [n = 0]() mutable { return n++; });
	pContext->WriteBuffer(m_pDeadList, deadList.data(), sizeof(uint32) * deadList.size());
	uint32 aliveCount = cMaxParticleCount;
	pContext->WriteBuffer(m_pCountersBuffer, &aliveCount, sizeof(uint32), 0);

	m_pParticleBuffer = pDevice->CreateBuffer(BufferDesc::CreateStructured(cMaxParticleCount, sizeof(ParticleData)), "Particle Buffer");

	m_pEmitArguments = pDevice->CreateBuffer(BufferDesc::CreateIndirectArguments<uint32>(3), "Emit Indirect Arguments");
	m_pSimulateArguments = pDevice->CreateBuffer(BufferDesc::CreateIndirectArguments<uint32>(3), "Simulate Indirect Arguments");
	m_pDrawArguments = pDevice->CreateBuffer(BufferDesc::CreateIndirectArguments<uint32>(4), "Draw Indirect Arguments");

	pContext->Execute(true);

	{
		m_pSimulateRS = new RootSignature(pDevice);
		m_pSimulateRS->AddRootConstants(0, 4);
		m_pSimulateRS->AddConstantBufferView(100);
		m_pSimulateRS->AddDescriptorTableSimple(0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8);
		m_pSimulateRS->AddDescriptorTableSimple(0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8);
		m_pSimulateRS->Finalize("Particle Simulation");
	}
	{
		m_pPrepareArgumentsPS = pDevice->CreateComputePipeline(m_pSimulateRS, "ParticleSimulation.hlsl", "UpdateSimulationParameters");
		m_pEmitPS = pDevice->CreateComputePipeline(m_pSimulateRS, "ParticleSimulation.hlsl", "Emit");
		m_pSimulatePS = pDevice->CreateComputePipeline(m_pSimulateRS, "ParticleSimulation.hlsl", "Simulate");
		m_pSimulateEndPS = pDevice->CreateComputePipeline(m_pSimulateRS, "ParticleSimulation.hlsl", "SimulateEnd");
	}
	{
		m_pRenderParticlesRS = new RootSignature(pDevice);
		m_pRenderParticlesRS->AddConstantBufferView(100);
		m_pRenderParticlesRS->AddDescriptorTableSimple(0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8);
		m_pRenderParticlesRS->Finalize("Particle Rendering");

		PipelineStateInitializer psoDesc;
		psoDesc.SetVertexShader("ParticleRendering.hlsl", "VSMain");
		psoDesc.SetPixelShader("ParticleRendering.hlsl", "PSMain");
		psoDesc.SetRootSignature(m_pRenderParticlesRS);
		psoDesc.SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		psoDesc.SetDepthWrite(false);
		psoDesc.SetBlendMode(BlendMode::Alpha, false);
		psoDesc.SetCullMode(D3D12_CULL_MODE_NONE);
		psoDesc.SetDepthTest(D3D12_COMPARISON_FUNC_GREATER);
		psoDesc.SetRenderTargetFormats(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT, 1);
		psoDesc.SetName("Particle Rendering PS");
		m_pRenderParticlesPS = pDevice->CreatePipeline(psoDesc);
	}
}

void GpuParticles::Simulate(RGGraph& graph, const SceneView& view, RGResourceHandle depth)
{
	if (ImGui::Begin("Parameters"))
	{
		if (ImGui::CollapsingHeader("Particles"))
		{
			ImGui::Checkbox("Enabled", &g_Enabled);
			ImGui::Checkbox("Simulate", &g_Simulate);
			ImGui::SliderInt("Emit Count", &g_EmitCount, 0, cMaxParticleCount / 50);
			ImGui::SliderFloat("Life Time", &g_LifeTime, 0, 10);
		}
	}
	ImGui::End();

	if (!g_Simulate || !g_Enabled)
	{
		return;
	}

	const ResourceView* uavs[] = {
		m_pCountersBuffer->GetUAV(),
		m_pEmitArguments->GetUAV(),
		m_pSimulateArguments->GetUAV(),
		m_pDrawArguments->GetUAV(),
		m_pDeadList->GetUAV(),
		m_pAliveList1->GetUAV(),
		m_pAliveList2->GetUAV(),
		m_pParticleBuffer->GetUAV(),
	};

	RG_GRAPH_SCOPE("Particle Simulation", graph);

	graph.AddPass("Prepare Arguments", RGPassFlag::Compute)
		.Read(depth)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
		{
			m_ParticlesToSpawn += (float)g_EmitCount * Time::DeltaTime();

			context.InsertResourceBarrier(m_pDrawArguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			context.InsertResourceBarrier(m_pEmitArguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			context.InsertResourceBarrier(m_pSimulateArguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			context.InsertResourceBarrier(m_pCountersBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			context.InsertResourceBarrier(m_pAliveList1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			context.InsertResourceBarrier(m_pAliveList2, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			context.InsertResourceBarrier(m_pParticleBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			context.SetComputeRootSignature(m_pSimulateRS);

			context.SetPipelineState(m_pPrepareArgumentsPS);
			struct
			{
				int32 EmitCount;
			} parameters;
			parameters.EmitCount = (int32)Math::Floor(m_ParticlesToSpawn);
			m_ParticlesToSpawn -= parameters.EmitCount;

			context.SetRootConstants(0, parameters);
			context.SetRootCBV(1, Renderer::GetViewUniforms(view));

			context.BindResources(2, uavs);
			context.BindResources(3,
				{
					m_pCountersBuffer->GetSRV(),
					resources.Get<Texture>(depth)->GetSRV(),
				});

			context.Dispatch(1);
			context.InsertUavBarrier();
			context.InsertResourceBarrier(m_pEmitArguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			context.InsertResourceBarrier(m_pSimulateArguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
		});

	graph.AddPass("Emit", RGPassFlag::Compute)
		.Read(depth)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
		{
			context.SetComputeRootSignature(m_pSimulateRS);
			context.SetPipelineState(m_pEmitPS);

			struct
			{
				Vector3 Origin;
			} parameters;

			parameters.Origin = Vector3(150, 3, 0);

			context.SetRootConstants(0, parameters);
			context.SetRootCBV(1, Renderer::GetViewUniforms(view));

			context.BindResources(2, uavs);
			context.BindResources(3,
				{
					m_pCountersBuffer->GetSRV(),
					resources.Get<Texture>(depth)->GetSRV(),
				});

			context.ExecuteIndirect(GraphicsCommon::pIndirectDispatchSignature, 1, m_pEmitArguments, m_pEmitArguments);
			context.InsertUavBarrier();
		});

	graph.AddPass("Simulate", RGPassFlag::Compute)
		.Read(depth)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
		{
			context.SetComputeRootSignature(m_pSimulateRS);
			context.SetPipelineState(m_pSimulatePS);

			struct
			{
				float DeltaTime;
				float ParticleLifeTime;
			} parameters;
			parameters.DeltaTime = Time::DeltaTime();
			parameters.ParticleLifeTime = g_LifeTime;

			context.SetRootConstants(0, parameters);
			context.SetRootCBV(1, Renderer::GetViewUniforms(view));

			context.BindResources(2, uavs);
			context.BindResources(3,
				{
					m_pCountersBuffer->GetSRV(),
					resources.Get<Texture>(depth)->GetSRV(),
				});

			context.ExecuteIndirect(GraphicsCommon::pIndirectDispatchSignature, 1, m_pSimulateArguments, nullptr);
			context.InsertUavBarrier();
			});

	graph.AddPass("Simulate End", RGPassFlag::Compute)
		.Read(depth)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
			{
				context.InsertResourceBarrier(m_pCountersBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

				context.SetComputeRootSignature(m_pSimulateRS);

				context.SetRootCBV(1, Renderer::GetViewUniforms(view));

				context.BindResources(2, uavs);
				context.BindResources(3,
					{
						m_pCountersBuffer->GetSRV(),
						resources.Get<Texture>(depth)->GetSRV(),
					});

			context.SetPipelineState(m_pSimulateEndPS);
			context.Dispatch(1);
			context.InsertUavBarrier();
		});

	std::swap(m_pAliveList1, m_pAliveList2);
}

void GpuParticles::Render(RGGraph& graph, const SceneView& view, SceneTextures& sceneTextures)
{
	if (!g_Enabled)
	{
		return;
	}

	graph.AddPass("Render Particles", RGPassFlag::Raster)
		.DepthStencil(sceneTextures.Depth, RenderPassAccess::Load_Store, false)
		.RenderTarget(sceneTextures.ColorTarget, RenderPassAccess::Load_Store)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
			{
				Texture* pTarget = resources.Get<Texture>(sceneTextures.ColorTarget);
				context.InsertResourceBarrier(m_pDrawArguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
				context.InsertResourceBarrier(m_pParticleBuffer, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
				context.InsertResourceBarrier(m_pAliveList1, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

				context.BeginRenderPass(resources.GetRenderPassInfo());

				context.SetPipelineState(m_pRenderParticlesPS);
				context.SetGraphicsRootSignature(m_pRenderParticlesRS);

				context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				context.SetRootCBV(0, Renderer::GetViewUniforms(view, pTarget));

				context.BindResources(1, {
					m_pParticleBuffer->GetSRV(),
					m_pAliveList1->GetSRV()
					});
				context.ExecuteIndirect(GraphicsCommon::pIndirectDrawSignature, 1, m_pDrawArguments, nullptr);
				context.EndRenderPass();
			});
}
