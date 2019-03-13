#pragma once

class CommandQueue;
class CommandContext;
class DescriptorAllocator;
class DynamicResourceAllocator;
class ImGuiRenderer;
class GraphicsBuffer;
class GraphicsResource;
class RootSignature;
class Texture2D;
class GraphicsPipelineState;
class ComputePipelineState;
class Mesh;

class Graphics
{
public:
	Graphics(uint32 width, uint32 height);
	~Graphics();

	virtual void Initialize(HWND window);
	virtual void Update();
	virtual void Shutdown();

	ID3D12Device* GetDevice() const { return m_pDevice.Get(); }
	void OnResize(int width, int height);

	bool IsFenceComplete(uint64 fenceValue);
	void WaitForFence(uint64 fenceValue);
	void IdleGPU();

	CommandQueue* GetCommandQueue(D3D12_COMMAND_LIST_TYPE type) const;
	CommandContext* AllocateCommandContext(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);
	void FreeCommandList(CommandContext* pCommandList);

	uint32 GetWindowWidth() const { return m_WindowWidth; }
	uint32 GetWindowHeight() const { return m_WindowHeight; }

	DynamicResourceAllocator* GetCpuVisibleAllocator() const { return m_pDynamicCpuVisibleAllocator.get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE AllocateCpuDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type);

	Texture2D* GetDepthStencilView() const { return m_pDepthStencilBuffer.get(); }
	Texture2D* GetCurrentRenderTarget() const {	return m_RenderTargets[m_CurrentBackBufferIndex].get();	}

	static const int32 FRAME_COUNT = 3;
	static const DXGI_FORMAT DEPTH_STENCIL_FORMAT;
	static const DXGI_FORMAT RENDER_TARGET_FORMAT;

private:
	uint64 GetFenceToWaitFor();

	void InitD3D();
	void InitializeAssets();

	void UpdateImGui();

	std::vector<float> m_FrameTimes;

	Vector3 m_CameraPosition;
	Quaternion m_CameraRotation;

	HWND m_pWindow = nullptr;

	ComPtr<IDXGIFactory3> m_pFactory;
	ComPtr<IDXGISwapChain3> m_pSwapchain;
	ComPtr<ID3D12Device> m_pDevice;

	std::array<std::unique_ptr<Texture2D>, FRAME_COUNT> m_RenderTargets;
	std::unique_ptr<Texture2D> m_pDepthStencilBuffer;

	std::array<std::unique_ptr<DescriptorAllocator>, D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES> m_DescriptorHeaps;
	std::unique_ptr<DynamicResourceAllocator> m_pDynamicCpuVisibleAllocator;
	std::array<std::unique_ptr<CommandQueue>, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE> m_CommandQueues;
	std::array<std::vector<std::unique_ptr<CommandContext>>, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE> m_CommandListPool;
	std::array < std::queue<CommandContext*>, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE> m_FreeCommandLists;
	std::vector<ComPtr<ID3D12CommandList>> m_CommandLists;
	std::mutex m_ContextAllocationMutex;

	std::unique_ptr<ImGuiRenderer> m_pImGuiRenderer;

	FloatRect m_Viewport;
	FloatRect m_ScissorRect;
	unsigned int m_WindowWidth;
	unsigned int m_WindowHeight;

	// Synchronization objects.
	uint32 m_CurrentBackBufferIndex = 0;
	std::array<uint64, FRAME_COUNT> m_FenceValues = {};

	std::unique_ptr<Mesh> m_pMesh;
	std::unique_ptr<RootSignature> m_pRootSignature;
	std::unique_ptr<GraphicsPipelineState> m_pPipelineStateObject;

	std::unique_ptr<Texture2D> m_pShadowMap;
	std::unique_ptr<RootSignature> m_pShadowsRootSignature;
	std::unique_ptr<GraphicsPipelineState> m_pShadowsPipelineStateObject;
};