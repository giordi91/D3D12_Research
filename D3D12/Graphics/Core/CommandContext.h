#pragma once
#include "DynamicResourceAllocator.h"
#include "GraphicsResource.h"
#include "OnlineDescriptorAllocator.h"
class Graphics;
class GraphicsResource;
class Texture;
class OnlineDescriptorAllocator;
class RootSignature;
class PipelineState;
class DynamicResourceAllocator;
class Buffer;
class CommandSignature;
class ShaderBindingTable;
struct BufferView;

enum class CommandListContext
{
	Graphics,
	Compute
};

enum class RenderTargetLoadAction : uint8
{
	DontCare,
	Load,
	Clear,
	NoAccess
};
DEFINE_ENUM_FLAG_OPERATORS(RenderTargetLoadAction)

enum class RenderTargetStoreAction : uint8
{
	DontCare,
	Store,
	Resolve,
	NoAccess,
};
DEFINE_ENUM_FLAG_OPERATORS(RenderTargetStoreAction)

enum class RenderPassAccess : uint8
{
#define COMBINE_ACTIONS(load, store) (uint8)RenderTargetLoadAction::load << 4 | (uint8)RenderTargetStoreAction::store
	DontCare_DontCare = COMBINE_ACTIONS(DontCare, DontCare),
	DontCare_Store = COMBINE_ACTIONS(DontCare, Store),
	Clear_Store = COMBINE_ACTIONS(Clear, Store),
	Load_Store = COMBINE_ACTIONS(Load, Store),
	Clear_DontCare = COMBINE_ACTIONS(Clear, DontCare),
	Load_DontCare = COMBINE_ACTIONS(Load, DontCare),
	Clear_Resolve = COMBINE_ACTIONS(Clear, Resolve),
	Load_Resolve = COMBINE_ACTIONS(Load, Resolve),
	DontCare_Resolve = COMBINE_ACTIONS(DontCare, Resolve),
	NoAccess = COMBINE_ACTIONS(NoAccess, NoAccess),
#undef COMBINE_ACTIONS
};

struct RenderPassInfo
{
	struct RenderTargetInfo
	{
		RenderPassAccess Access = RenderPassAccess::DontCare_DontCare;
		Texture* Target = nullptr;
		Texture* ResolveTarget = nullptr;
		int MipLevel = 0;
		int ArrayIndex = 0;
	};

	struct DepthTargetInfo
	{
		RenderPassAccess Access = RenderPassAccess::DontCare_DontCare;
		RenderPassAccess StencilAccess = RenderPassAccess::DontCare_DontCare;
		Texture* Target = nullptr;
	};

	RenderPassInfo()
	{
	}

	RenderPassInfo(Texture* pDepthBuffer, RenderPassAccess access, bool uavWrites = false)
		: RenderTargetCount(0)
	{
		DepthStencilTarget.Access = access;
		DepthStencilTarget.Target = pDepthBuffer;
		DepthStencilTarget.StencilAccess = RenderPassAccess::NoAccess;
		WriteUAVs = uavWrites;
	}

	RenderPassInfo(Texture* pRenderTarget, RenderPassAccess renderTargetAccess, Texture* pDepthBuffer, RenderPassAccess depthAccess, bool uavWrites = false, RenderPassAccess stencilAccess = RenderPassAccess::NoAccess)
		: RenderTargetCount(1)
	{
		RenderTargets[0].Access = renderTargetAccess;
		RenderTargets[0].Target = pRenderTarget;
		DepthStencilTarget.Access = depthAccess;
		DepthStencilTarget.Target = pDepthBuffer;
		DepthStencilTarget.StencilAccess = stencilAccess;
		WriteUAVs = uavWrites;
	}

	static RenderTargetLoadAction GetBeginAccess(RenderPassAccess access)
	{
		return (RenderTargetLoadAction)((uint8)access >> 4);
	}

	static RenderTargetStoreAction GetEndAccess(RenderPassAccess access)
	{
		return (RenderTargetStoreAction)((uint8)access & 0b1111);
	}

	bool WriteUAVs = false;
	uint32 RenderTargetCount = 0;
	std::array<RenderTargetInfo, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> RenderTargets{};
	DepthTargetInfo DepthStencilTarget{};
};

class ResourceBarrierBatcher
{
public:
	void AddTransition(ID3D12Resource* pResource, D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState, int subResource);
	void AddUAV(ID3D12Resource* pResource);
	void Flush(ID3D12GraphicsCommandList* pCmdList);
	void Reset();
	bool HasWork() const { return m_QueuedBarriers.size() > 0; }

private:
	std::vector<D3D12_RESOURCE_BARRIER> m_QueuedBarriers;
};

class CommandContext : public GraphicsObject
{
public:
	CommandContext(Graphics* pGraphics, ID3D12GraphicsCommandList* pCommandList, D3D12_COMMAND_LIST_TYPE type);
	~CommandContext();

	void Reset();
	uint64 Execute(bool wait);
	static uint64 Execute(CommandContext** pContexts, uint32 numContexts, bool wait);
	void Free(uint64 fenceValue);

	void InsertResourceBarrier(GraphicsResource* pBuffer, D3D12_RESOURCE_STATES state, uint32 subResource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	void InsertUavBarrier(GraphicsResource* pBuffer = nullptr);
	void FlushResourceBarriers();

	void CopyTexture(GraphicsResource* pSource, GraphicsResource* pTarget);
	void CopyTexture(Texture* pSource, Buffer* pDestination, const D3D12_BOX& sourceRegion, int sourceSubregion = 0, int destinationOffset = 0);
	void CopyTexture(Texture* pSource, Texture* pDestination, const D3D12_BOX& sourceRegion, const D3D12_BOX& destinationRegion, int sourceSubregion = 0, int destinationSubregion = 0);
	void CopyBuffer(Buffer* pSource, Buffer* pDestination, uint32 size, uint32 sourceOffset, uint32 destinationOffset);
	void InitializeBuffer(Buffer* pResource, const void* pData, uint64 dataSize, uint64 offset = 0);
	void InitializeTexture(Texture* pResource, D3D12_SUBRESOURCE_DATA* pSubResourceDatas, int firstSubResource, int subResourceCount);

	ID3D12GraphicsCommandList* GetCommandList() const { return m_pCommandList; }
	ID3D12GraphicsCommandList4* GetRaytracingCommandList() const { return  m_pRaytracingCommandList.Get(); }
	ID3D12GraphicsCommandList6* GetMeshShadingCommandList() const { return  m_pMeshShadingCommandList.Get(); }

	D3D12_COMMAND_LIST_TYPE GetType() const { return m_Type; }

	//Commands
	void Dispatch(uint32 groupCountX, uint32 groupCountY = 1, uint32 groupCountZ = 1);
	void Dispatch(const IntVector3& groupCounts);
	void DispatchMesh(uint32 groupCountX, uint32 groupCountY = 1, uint32 groupCountZ = 1);
	void ExecuteIndirect(CommandSignature* pCommandSignature, Buffer* pIndirectArguments, bool isCompute = true);
	void Draw(int vertexStart, int vertexCount);
	void DrawIndexed(int indexCount, int indexStart, int minVertex = 0);
	void DrawIndexedInstanced(int indexCount, int indexStart, int instanceCount, int minVertex = 0, int instanceStart = 0);
	
	void DispatchRays(ShaderBindingTable& table, uint32 width = 1, uint32 height = 1, uint32 depth = 1);

	void ClearColor(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const Color& color = Color(0.0f, 0.0f, 0.0f, 1.0f));
	void ClearDepth(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS clearFlags = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, float depth = 1.0f, unsigned char stencil = 0);
	void ResolveResource(Texture* pSource, uint32 sourceSubResource, Texture* pTarget, uint32 targetSubResource, DXGI_FORMAT format);

	void PrepareDraw(DescriptorTableType type);

	void BeginRenderPass(const RenderPassInfo& renderPassInfo);
	void EndRenderPass();

	void ClearUavUInt(GraphicsResource* pBuffer, UnorderedAccessView* pUav, uint32* values = nullptr);
	void ClearUavFloat(GraphicsResource* pBuffer, UnorderedAccessView* pUav, float* values = nullptr);

	//Bindings
	void SetPipelineState(PipelineState* pPipelineState);
	void SetPipelineState(ID3D12StateObject* pStateObject);
	void SetComputeRootSignature(RootSignature* pRootSignature);
	void SetComputeRootConstants(int rootIndex, uint32 count, const void* pConstants);
	void SetComputeDynamicConstantBufferView(int rootIndex, void* pData, uint32 dataSize);

	void SetDynamicDescriptor(int rootIndex, int offset, D3D12_CPU_DESCRIPTOR_HANDLE handle);
	void SetDynamicDescriptor(int rootIndex, int offset, UnorderedAccessView* pView);
	void SetDynamicDescriptor(int rootIndex, int offset, ShaderResourceView* pView);
	void SetDynamicDescriptors(int rootIndex, int offset, const D3D12_CPU_DESCRIPTOR_HANDLE* handles, int count = 1);
	void SetDynamicSampler(int rootIndex, int offset, D3D12_CPU_DESCRIPTOR_HANDLE handle);
	void SetDynamicSamplers(int rootIndex, int offset, const D3D12_CPU_DESCRIPTOR_HANDLE* handles, int count = 1);

	void SetGraphicsRootSignature(RootSignature* pRootSignature);

	void SetGraphicsRootConstants(int rootIndex, uint32 count, const void* pConstants);
	void SetDynamicConstantBufferView(int rootIndex, const void* pData, uint32 dataSize);
	void SetDynamicVertexBuffer(int slot, int elementCount, int elementSize, const void* pData);
	void SetDynamicIndexBuffer(int elementCount, const void* pData, bool smallIndices = false);
	void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY type);
	void SetVertexBuffer(const VertexBufferView& buffer);
	void SetVertexBuffers(const VertexBufferView* pBuffers, int bufferCount);
	void SetIndexBuffer(const IndexBufferView& indexBuffer);
	void SetViewport(const FloatRect& rect, float minDepth = 0.0f, float maxDepth = 1.0f);
	void SetScissorRect(const FloatRect& rect);

	void SetDescriptorHeap(ID3D12DescriptorHeap* pHeap, D3D12_DESCRIPTOR_HEAP_TYPE type);

	void SetShadingRate(D3D12_SHADING_RATE shadingRate = D3D12_SHADING_RATE_1X1);

	DynamicAllocation AllocateTransientMemory(uint64 size);
	DescriptorHandle AllocateTransientDescriptors(int descriptorCount, D3D12_DESCRIPTOR_HEAP_TYPE type);

	struct PendingBarrier
	{
		GraphicsResource* pResource;
		ResourceState State;
		uint32 Subresource;
	};
	const std::vector<PendingBarrier>& GetPendingBarriers() const { return m_PendingBarriers; }

	D3D12_RESOURCE_STATES GetResourceState(GraphicsResource* pResource, uint32 subResource) const 
	{
		auto it = m_ResourceStates.find(pResource);
		check(it != m_ResourceStates.end());
		return it->second.Get(subResource);
	}

	D3D12_RESOURCE_STATES GetResourceStateWithFallback(GraphicsResource* pResource, uint32 subResource) const
	{
		auto it = m_ResourceStates.find(pResource);
		if (it == m_ResourceStates.end())
		{
			return pResource->GetResourceState(subResource);
		}
		return it->second.Get(subResource);
	}

	static bool IsTransitionAllowed(D3D12_COMMAND_LIST_TYPE commandlistType, D3D12_RESOURCE_STATES state);

private:
	void BindDescriptorHeaps();

	std::unique_ptr<OnlineDescriptorAllocator> m_pShaderResourceDescriptorAllocator;
	std::unique_ptr<OnlineDescriptorAllocator> m_pSamplerDescriptorAllocator;

	std::array<ID3D12DescriptorHeap*, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> m_CurrentDescriptorHeaps = {};

	ResourceBarrierBatcher m_BarrierBatcher;

	std::unique_ptr<DynamicResourceAllocator> m_DynamicAllocator;
	ID3D12GraphicsCommandList* m_pCommandList;
	ComPtr<ID3D12GraphicsCommandList4> m_pRaytracingCommandList;
	ComPtr<ID3D12GraphicsCommandList6> m_pMeshShadingCommandList;
	ID3D12CommandAllocator* m_pAllocator;
	D3D12_COMMAND_LIST_TYPE m_Type;
	std::unordered_map<GraphicsResource*, ResourceState> m_ResourceStates;
	std::vector<PendingBarrier> m_PendingBarriers;

	std::array<D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> m_ResolveSubResourceParameters{};
	RenderPassInfo m_CurrentRenderPassInfo;
	bool m_InRenderPass = false;
};

class ScopedBarrier
{
public:
	ScopedBarrier(CommandContext& context, GraphicsResource* pResource, D3D12_RESOURCE_STATES state, uint32 subResources = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) : m_Context(context), m_pResource(pResource), m_Subresources(subResources)
	{
		m_BeforeState = context.GetResourceStateWithFallback(pResource, subResources);
		context.InsertResourceBarrier(pResource, state, subResources);
	}

	~ScopedBarrier()
	{
		m_Context.InsertResourceBarrier(m_pResource, m_BeforeState, m_Subresources);
	}
private:
	CommandContext& m_Context;
	GraphicsResource* m_pResource;
	uint32 m_Subresources;
	D3D12_RESOURCE_STATES m_BeforeState = D3D12_RESOURCE_STATE_UNKNOWN;
};

class CommandSignature
{
public:
	void Finalize(const char* pName, ID3D12Device* pDevice);

	void SetRootSignature(ID3D12RootSignature* pRootSignature) { m_pRootSignature = pRootSignature; }
	void AddDispatch();
	void AddDraw();
	void AddDrawIndexed();

	ID3D12CommandSignature* GetCommandSignature() const { return m_pCommandSignature.Get(); }

private:
	ComPtr<ID3D12CommandSignature> m_pCommandSignature;
	ID3D12RootSignature* m_pRootSignature = nullptr;
	uint32 m_Stride = 0;
	std::vector<D3D12_INDIRECT_ARGUMENT_DESC> m_ArgumentDesc;
};