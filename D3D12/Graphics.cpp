#include "stdafx.h"
#include "Graphics.h"
#include <map>
#include <fstream>
#include "CommandAllocatorPool.h"
#include "CommandQueue.h"
#include "CommandContext.h"

#include "DescriptorAllocator.h"
#include "GraphicsResource.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Shader.h"
#include "Mesh.h"
#include "DynamicResourceAllocator.h"
#include "ImGuiRenderer.h"
#include "External/Imgui/imgui.h"
#include "Input.h"

const DXGI_FORMAT Graphics::DEPTH_STENCIL_FORMAT = DXGI_FORMAT_D24_UNORM_S8_UINT;
const DXGI_FORMAT Graphics::RENDER_TARGET_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

#pragma pack(push)
#pragma pack(16) 
struct Light
{
	int Enabled;
	Vector3 Position;
	Vector3 Direction;
	float Intensity;
	Vector4 Color;
	float Range;
	float SpotLightAngle;
	float Attenuation;
	uint32 Type;

	static Light Directional(const Vector3& position, const Vector3& direction, float intensity = 1.0f, const Vector4& color = Vector4(1, 1, 1, 1))
	{
		Light l;
		l.Enabled = true;
		l.Position = position;
		l.Direction = direction;
		l.Intensity = intensity;
		l.Color = color;
		l.Type = 0;
		return l;
	}

	static Light Point(const Vector3& position, float radius, float intensity = 1.0f, float attenuation = 0.5f, const Vector4& color = Vector4(1, 1, 1, 1))
	{
		Light l;
		l.Enabled = true;
		l.Position = position;
		l.Range = radius;
		l.Intensity = intensity;
		l.Color = color;
		l.Attenuation = attenuation;
		l.Type = 1;
		return l;
	}
};
#pragma pack(pop)

Graphics::Graphics(uint32 width, uint32 height)
	: m_WindowWidth(width), m_WindowHeight(height)
{
}

Graphics::~Graphics()
{
}

void Graphics::Initialize(HWND window)
{
	m_pWindow = window;
	InitD3D();
	InitializeAssets();

	m_FrameTimes.resize(256);

	m_CameraPosition = Vector3(0, 100, -15);
	m_CameraRotation = Quaternion::CreateFromYawPitchRoll(XM_PIDIV4, XM_PIDIV4, 0);
}

void Graphics::Update()
{
	Vector3 mainLightPosition = Vector3(cos((float)GameTimer::GameTime() / 5.0f), 1.5, sin((float)GameTimer::GameTime() / 5.0f)) * 80;
	Vector3 mainLightDirection;
	mainLightPosition.Normalize(mainLightDirection);
	mainLightDirection *= -1;

	std::vector<Light> lights;
	lights.push_back(Light::Directional(mainLightPosition, mainLightDirection));
	lights.push_back(Light::Point(Vector3(-100, 20, 0), 50.0f, 1.0f, 0.5f, Vector4(1, 0, 0, 1)));
	lights.push_back(Light::Point(Vector3(0, 20, 0), 50.0f, 1.0f, 0.5f, Vector4(0, 0, 1, 1)));
	lights.push_back(Light::Point(Vector3(100, 70, 50), 50.0f, 1.0f, 0.5f, Vector4(0, 1, 1, 1)));
	lights.push_back(Light::Point(Vector3(100, 70, -50), 50.0f, 1.0f, 0.5f, Vector4(1, 1, 0, 1)));

	struct PerFrameData
	{
		Matrix LightViewProjection;
		Matrix ViewInverse;
	} frameData;

	frameData.LightViewProjection = XMMatrixLookAtLH(lights[0].Position, Vector3(0, 0, 0), Vector3(0, 1, 0)) * XMMatrixOrthographicLH(512, 512, 5.0f, 200.0f);

	if (Input::Instance().IsMouseDown(VK_LBUTTON))
	{
		Vector2 mouseDelta = Input::Instance().GetMouseDelta();
		Quaternion yr = Quaternion::CreateFromYawPitchRoll(0, mouseDelta.y * GameTimer::DeltaTime() * 0.1f, 0);
		Quaternion pr = Quaternion::CreateFromYawPitchRoll(mouseDelta.x * GameTimer::DeltaTime() * 0.1f, 0, 0);
		m_CameraRotation = yr * m_CameraRotation * pr;
	}

	Vector3 movement;
	movement.x -= (int)Input::Instance().IsKeyDown('A');
	movement.x += (int)Input::Instance().IsKeyDown('D');
	movement.z -= (int)Input::Instance().IsKeyDown('S');
	movement.z += (int)Input::Instance().IsKeyDown('W');
	movement = Vector3::Transform(movement, m_CameraRotation);
	movement.y -= (int)Input::Instance().IsKeyDown('Q');
	movement.y += (int)Input::Instance().IsKeyDown('E');
	movement *= GameTimer::DeltaTime() * 20.0f;
	m_CameraPosition += movement;

	frameData.ViewInverse = Matrix::CreateFromQuaternion(m_CameraRotation) * Matrix::CreateTranslation(m_CameraPosition);
	Matrix cameraView;
	frameData.ViewInverse.Invert(cameraView);
	Matrix cameraProjection = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)m_WindowWidth / m_WindowHeight, 1.0f, 300);
	Matrix cameraViewProjection = cameraView * cameraProjection;

	m_pImGuiRenderer->NewFrame();

	uint64 nextFenceValue = 0;

	//Shadow Map
	{
		GraphicsCommandContext* pContext = (GraphicsCommandContext*)AllocateCommandContext(D3D12_COMMAND_LIST_TYPE_DIRECT);
		pContext->MarkBegin(L"Shadows");
		pContext->SetPipelineState(m_pShadowsPipelineStateObject.get());
		pContext->SetGraphicsRootSignature(m_pShadowsRootSignature.get());

		pContext->SetViewport(FloatRect(0, 0, m_pShadowMap->GetWidth(), m_pShadowMap->GetHeight()));
		pContext->SetScissorRect(FloatRect(0, 0, m_pShadowMap->GetWidth(), m_pShadowMap->GetHeight()));

		pContext->InsertResourceBarrier(m_pShadowMap.get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
		pContext->SetRenderTargets(nullptr, m_pShadowMap->GetRTV());

		Color clearColor = Color(0.1f, 0.1f, 0.1f, 1.0f);
		pContext->ClearDepth(m_pShadowMap->GetRTV(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0);

		pContext->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		struct PerObjectData
		{
			Matrix WorldViewProjection;
		} ObjectData;
		ObjectData.WorldViewProjection = frameData.LightViewProjection;
		pContext->SetDynamicConstantBufferView(0, &ObjectData, sizeof(PerObjectData));
		for (int i = 0; i < m_pMesh->GetMeshCount(); ++i)
		{
			m_pMesh->GetMesh(i)->Draw(pContext);
		}

		pContext->InsertResourceBarrier(m_pShadowMap.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
		pContext->MarkEnd();
		pContext->Execute(false);
	}

	//3D
	{
		GraphicsCommandContext* pContext = (GraphicsCommandContext*)AllocateCommandContext(D3D12_COMMAND_LIST_TYPE_DIRECT);
		pContext->MarkBegin(L"3D");
		pContext->SetPipelineState(m_pPipelineStateObject.get());
		pContext->SetGraphicsRootSignature(m_pRootSignature.get());

		pContext->SetViewport(m_Viewport);
		pContext->SetScissorRect(m_ScissorRect);

		pContext->InsertResourceBarrier(m_RenderTargets[m_CurrentBackBufferIndex].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, true);

		pContext->SetRenderTargets(&GetCurrentRenderTarget()->GetRTV(), GetDepthStencilView()->GetRTV());

		Color clearColor = Color(0.1f, 0.1f, 0.1f, 1.0f);
		pContext->ClearRenderTarget(GetCurrentRenderTarget()->GetRTV(), clearColor);
		pContext->ClearDepth(GetDepthStencilView()->GetRTV(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0);

		pContext->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		struct PerObjectData
		{
			Matrix World;
			Matrix WorldViewProjection;
		} ObjectData;
		ObjectData.World = XMMatrixIdentity();
		ObjectData.WorldViewProjection = ObjectData.World * cameraViewProjection;

		pContext->SetDynamicConstantBufferView(0, &ObjectData, sizeof(PerObjectData));
		pContext->SetDynamicConstantBufferView(1, &frameData, sizeof(PerFrameData));
		pContext->SetDynamicConstantBufferView(2, lights.data(), sizeof(Light) * lights.size());
		pContext->SetDynamicDescriptor(4, 0, m_pShadowMap->GetSRV());
		for (int i = 0; i < m_pMesh->GetMeshCount(); ++i)
		{
			SubMesh* pSubMesh = m_pMesh->GetMesh(i);
			const Material& material = m_pMesh->GetMaterial(pSubMesh->GetMaterialId());
			pContext->SetDynamicDescriptor(3, 0, material.pDiffuseTexture->GetSRV());
			pContext->SetDynamicDescriptor(3, 1, material.pNormalTexture->GetSRV());
			pContext->SetDynamicDescriptor(3, 2, material.pSpecularTexture->GetSRV());
			pSubMesh->Draw(pContext);
		}
		pContext->MarkEnd();
		pContext->Execute(false);
	}

	UpdateImGui();

	GraphicsCommandContext* pContext = (GraphicsCommandContext*)AllocateCommandContext(D3D12_COMMAND_LIST_TYPE_DIRECT);
	pContext->MarkBegin(L"UI");
	////UI
	{
		m_pImGuiRenderer->Render(*pContext);
	}
	pContext->MarkEnd();

	pContext->MarkBegin(L"Present");
	//Present
	{
		pContext->InsertResourceBarrier(m_RenderTargets[m_CurrentBackBufferIndex].get(), D3D12_RESOURCE_STATE_PRESENT, true);
	}
	pContext->MarkEnd();
	nextFenceValue = pContext->Execute(false);

	uint64 waitFenceIdx = GetFenceToWaitFor();
	WaitForFence(m_FenceValues[waitFenceIdx]);
	m_FenceValues[waitFenceIdx] = nextFenceValue;
	m_pSwapchain->Present(1, 0);
	m_CurrentBackBufferIndex = m_pSwapchain->GetCurrentBackBufferIndex();
}

void Graphics::Shutdown()
{
	// Wait for the GPU to be done with all resources.
	IdleGPU();
}

uint64 Graphics::GetFenceToWaitFor()
{
	return (m_CurrentBackBufferIndex + (FRAME_COUNT - 1)) % FRAME_COUNT;
}

void Graphics::InitD3D()
{
	UINT dxgiFactoryFlags = 0;

#ifdef _DEBUG
	//Enable debug
	ComPtr<ID3D12Debug> pDebugController;
	HR(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebugController)));
	pDebugController->EnableDebugLayer();
	// Enable additional debug layers.
	dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

	//Create the factory
	HR(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_pFactory)));

	//Create the device
	HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_pDevice)));

#ifdef _DEBUG
	ID3D12InfoQueue* pInfoQueue = nullptr;
	if (HR(m_pDevice->QueryInterface(IID_PPV_ARGS(&pInfoQueue))))
	{
		// Suppress whole categories of messages
		//D3D12_MESSAGE_CATEGORY Categories[] = {};

		// Suppress messages based on their severity level
		D3D12_MESSAGE_SEVERITY Severities[] =
		{
			D3D12_MESSAGE_SEVERITY_INFO
		};

		// Suppress individual messages by their ID
		D3D12_MESSAGE_ID DenyIds[] =
		{
			// This occurs when there are uninitialized descriptors in a descriptor table, even when a
			// shader does not access the missing descriptors.  I find this is common when switching
			// shader permutations and not wanting to change much code to reorder resources.
			D3D12_MESSAGE_ID_INVALID_DESCRIPTOR_HANDLE,
		};

		D3D12_INFO_QUEUE_FILTER NewFilter = {};
		//NewFilter.DenyList.NumCategories = _countof(Categories);
		//NewFilter.DenyList.pCategoryList = Categories;
		NewFilter.DenyList.NumSeverities = _countof(Severities);
		NewFilter.DenyList.pSeverityList = Severities;
		NewFilter.DenyList.NumIDs = _countof(DenyIds);
		NewFilter.DenyList.pIDList = DenyIds;

		pInfoQueue->PushStorageFilter(&NewFilter);
		pInfoQueue->Release();
	}
#endif

	//Check 4x MSAA support
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qualityLevels;
	qualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	qualityLevels.Format = RENDER_TARGET_FORMAT;
	qualityLevels.NumQualityLevels = 0;
	qualityLevels.SampleCount = 1;
	HR(m_pDevice->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qualityLevels, sizeof(qualityLevels)));

	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_DIRECT] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_DIRECT);
	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_COMPUTE] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_COMPUTE);

	assert(m_DescriptorHeaps.size() == D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES);
	for (size_t i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++i)
	{
		m_DescriptorHeaps[i] = std::make_unique<DescriptorAllocator>(m_pDevice.Get(), (D3D12_DESCRIPTOR_HEAP_TYPE)i);
	}
	m_pDynamicCpuVisibleAllocator = std::make_unique<DynamicResourceAllocator>(this, true, 0x400000);

	m_pSwapchain.Reset();

	DXGI_SWAP_CHAIN_DESC1 swapchainDesc = {};
	swapchainDesc.Width = m_WindowWidth;
	swapchainDesc.Height = m_WindowHeight;
	swapchainDesc.Format = RENDER_TARGET_FORMAT;
	swapchainDesc.SampleDesc.Count = 1;
	swapchainDesc.SampleDesc.Quality = 0;
	swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapchainDesc.BufferCount = FRAME_COUNT;
	swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	swapchainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	swapchainDesc.Stereo = false;
	ComPtr<IDXGISwapChain1> swapChain;

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc{};
	fsDesc.RefreshRate.Denominator = 60;
	fsDesc.RefreshRate.Numerator = 1;
	fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	fsDesc.Windowed = true;
	HR(m_pFactory->CreateSwapChainForHwnd(
		m_CommandQueues[D3D12_COMMAND_LIST_TYPE_DIRECT]->GetCommandQueue(), 
		m_pWindow, 
		&swapchainDesc, 
		&fsDesc, 
		nullptr, 
		&swapChain));

	swapChain.As(&m_pSwapchain);
	OnResize(m_WindowWidth, m_WindowHeight);

	m_pImGuiRenderer = std::make_unique<ImGuiRenderer>(this);
}

void Graphics::OnResize(int width, int height)
{
	m_WindowWidth = width;
	m_WindowHeight = height;

	IdleGPU();
	for (int i = 0; i < FRAME_COUNT; ++i)
	{
		m_RenderTargets[i].reset();
	}
	m_pDepthStencilBuffer.reset();

	//Resize the buffers
	HR(m_pSwapchain->ResizeBuffers(
		FRAME_COUNT, 
		m_WindowWidth, 
		m_WindowHeight, 
		RENDER_TARGET_FORMAT,
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

	m_CurrentBackBufferIndex = 0;

	//Recreate the render target views
	for (int i = 0; i < FRAME_COUNT; ++i)
	{
		ID3D12Resource* pResource = nullptr;
		HR(m_pSwapchain->GetBuffer(i, IID_PPV_ARGS(&pResource)));
		m_RenderTargets[i] = std::make_unique<Texture2D>();
		m_RenderTargets[i]->CreateForSwapchain(this, pResource);
	}

	m_pDepthStencilBuffer = std::make_unique<Texture2D>();
	m_pDepthStencilBuffer->Create(this, width, height, DEPTH_STENCIL_FORMAT, TextureUsage::DepthStencil);

	m_Viewport.Bottom = (float)m_WindowHeight;
	m_Viewport.Right = (float)m_WindowWidth;
	m_Viewport.Left = 0.0f;
	m_Viewport.Top = 0.0f;
	m_ScissorRect = m_Viewport;
}

void Graphics::InitializeAssets()
{
	GraphicsCommandContext* pContext = (GraphicsCommandContext*)AllocateCommandContext(D3D12_COMMAND_LIST_TYPE_DIRECT);

	//Input layout
	D3D12_INPUT_ELEMENT_DESC inputElements[] = {
		D3D12_INPUT_ELEMENT_DESC{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		D3D12_INPUT_ELEMENT_DESC{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		D3D12_INPUT_ELEMENT_DESC{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		D3D12_INPUT_ELEMENT_DESC{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		D3D12_INPUT_ELEMENT_DESC{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	{
		//Shaders
		Shader vertexShader;
		vertexShader.Load("Resources/Diffuse.hlsl", Shader::Type::VertexShader, "VSMain");
		Shader pixelShader;
		pixelShader.Load("Resources/Diffuse.hlsl", Shader::Type::PixelShader, "PSMain");

		//Rootsignature
		m_pRootSignature = std::make_unique<RootSignature>(5);
		m_pRootSignature->SetConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
		m_pRootSignature->SetConstantBufferView(1, 1, D3D12_SHADER_VISIBILITY_ALL);
		m_pRootSignature->SetConstantBufferView(2, 2, D3D12_SHADER_VISIBILITY_PIXEL);
		m_pRootSignature->SetDescriptorTableSimple(3, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, D3D12_SHADER_VISIBILITY_PIXEL);
		m_pRootSignature->SetDescriptorTableSimple(4, 3, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, D3D12_SHADER_VISIBILITY_PIXEL);

		D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		D3D12_SAMPLER_DESC samplerDesc = {};
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		m_pRootSignature->AddStaticSampler(0, samplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);

		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		m_pRootSignature->AddStaticSampler(1, samplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);

		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		samplerDesc.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
		m_pRootSignature->AddStaticSampler(2, samplerDesc, D3D12_SHADER_VISIBILITY_PIXEL);

		m_pRootSignature->Finalize(m_pDevice.Get(), rootSignatureFlags);

		//Pipeline state
		m_pPipelineStateObject = std::make_unique<GraphicsPipelineState>();
		m_pPipelineStateObject->SetInputLayout(inputElements, sizeof(inputElements) / sizeof(inputElements[0]));
		m_pPipelineStateObject->SetRootSignature(m_pRootSignature->GetRootSignature());
		m_pPipelineStateObject->SetVertexShader(vertexShader.GetByteCode(), vertexShader.GetByteCodeSize());
		m_pPipelineStateObject->SetPixelShader(pixelShader.GetByteCode(), pixelShader.GetByteCodeSize());
		m_pPipelineStateObject->SetRenderTargetFormat(RENDER_TARGET_FORMAT, DEPTH_STENCIL_FORMAT, 1, 0);
		m_pPipelineStateObject->Finalize(m_pDevice.Get());
	}

	{
		Shader vertexShader;
		vertexShader.Load("Resources/Shadows.hlsl", Shader::Type::VertexShader, "VSMain");

		//Rootsignature
		m_pShadowsRootSignature = std::make_unique<RootSignature>(1);
		m_pShadowsRootSignature->SetConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

		D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		m_pShadowsRootSignature->Finalize(m_pDevice.Get(), rootSignatureFlags);

		//Pipeline state
		m_pShadowsPipelineStateObject = std::make_unique<GraphicsPipelineState>();
		m_pShadowsPipelineStateObject->SetInputLayout(inputElements, sizeof(inputElements) / sizeof(inputElements[0]));
		m_pShadowsPipelineStateObject->SetRootSignature(m_pShadowsRootSignature->GetRootSignature());
		m_pShadowsPipelineStateObject->SetVertexShader(vertexShader.GetByteCode(), vertexShader.GetByteCodeSize());
		m_pShadowsPipelineStateObject->SetRenderTargetFormats(nullptr, 0, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 1, 0);
		m_pShadowsPipelineStateObject->SetCullMode(D3D12_CULL_MODE_NONE);
		m_pShadowsPipelineStateObject->Finalize(m_pDevice.Get());

		m_pShadowMap = std::make_unique<Texture2D>();
		m_pShadowMap->Create(this, 4096, 4096, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, TextureUsage::DepthStencil | TextureUsage::ShaderResource);
	}

	//Geometry
	m_pMesh = std::make_unique<Mesh>();
	m_pMesh->Load("Resources/sponza/sponza.dae", this, pContext);

	pContext->Execute(true);
}

void Graphics::UpdateImGui()
{
	for (int i = 1; i < m_FrameTimes.size(); ++i)
	{
		m_FrameTimes[i - 1] = m_FrameTimes[i];
	}
	m_FrameTimes[m_FrameTimes.size() - 1] = GameTimer::DeltaTime();

	ImGui::SetNextWindowPos(ImVec2(0, 0), 0, ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(250, m_WindowHeight));
	ImGui::Begin("GPU Stats", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	ImGui::Text("MS: %.4f", GameTimer::DeltaTime());
	ImGui::SameLine(100);
	ImGui::Text("FPS: %.1f", 1.0f / GameTimer::DeltaTime());
	ImGui::PlotLines("Frametime", m_FrameTimes.data(), m_FrameTimes.size(), 0, 0, 0.0f, 0.03f, ImVec2(200, 100));
	ImGui::BeginTabBar("GpuStatsBar");
	ImGui::BeginTabItem("Descriptor Heaps");
	ImGui::Text("Used CPU Descriptor Heaps");
	for (const auto& pAllocator : m_DescriptorHeaps)
	{
		switch (pAllocator->GetType())
		{
		case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
			ImGui::TextWrapped("Constant/Shader/Unordered Access Views");
			break;
		case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
			ImGui::TextWrapped("Samplers");
			break;
		case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
			ImGui::TextWrapped("Render Target Views");
			break;
		case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
			ImGui::TextWrapped("Depth Stencil Views");
			break;
		default:
			break;
		}
		uint32 totalDescriptors = pAllocator->GetHeapCount() * DescriptorAllocator::DESCRIPTORS_PER_HEAP;
		uint32 usedDescriptors = pAllocator->GetNumAllocatedDescriptors();
		std::stringstream str;
		str << usedDescriptors << "/" << totalDescriptors;
		ImGui::ProgressBar((float)usedDescriptors / totalDescriptors, ImVec2(-1, 0), str.str().c_str());
	}
	ImGui::EndTabItem();
	ImGui::EndTabBar();
	ImGui::End();
}

CommandQueue* Graphics::GetCommandQueue(D3D12_COMMAND_LIST_TYPE type) const
{
	return m_CommandQueues.at(type).get();
}

CommandContext* Graphics::AllocateCommandContext(D3D12_COMMAND_LIST_TYPE type)
{
	int typeIndex = (int)type;

	std::lock_guard<std::mutex> lockGuard(m_ContextAllocationMutex);
	if (m_FreeCommandLists[typeIndex].size() > 0)
	{
		CommandContext* pCommandList = m_FreeCommandLists[typeIndex].front();
		m_FreeCommandLists[typeIndex].pop();
		pCommandList->Reset();
		return pCommandList;
	}
	else
	{
		ComPtr<ID3D12CommandList> pCommandList;
		ID3D12CommandAllocator* pAllocator = m_CommandQueues[type]->RequestAllocator();
		m_pDevice->CreateCommandList(0, type, pAllocator, nullptr, IID_PPV_ARGS(pCommandList.GetAddressOf()));
		m_CommandLists.push_back(std::move(pCommandList));
		switch (type)
		{
		case D3D12_COMMAND_LIST_TYPE_DIRECT:
			m_CommandListPool[typeIndex].emplace_back(std::make_unique<GraphicsCommandContext>(this, static_cast<ID3D12GraphicsCommandList*>(m_CommandLists.back().Get()), pAllocator));
			break;
		case D3D12_COMMAND_LIST_TYPE_COMPUTE:
			m_CommandListPool[typeIndex].emplace_back(std::make_unique<ComputeCommandContext>(this, static_cast<ID3D12GraphicsCommandList*>(m_CommandLists.back().Get()), pAllocator));
			break;
		default:
			assert(false);
			break;
		}
		return m_CommandListPool[typeIndex].back().get();
	}
}

bool Graphics::IsFenceComplete(uint64 fenceValue)
{
	D3D12_COMMAND_LIST_TYPE type = (D3D12_COMMAND_LIST_TYPE)(fenceValue >> 56);
	CommandQueue* pQueue = GetCommandQueue(type);
	return pQueue->IsFenceComplete(fenceValue);
}

void Graphics::WaitForFence(uint64 fenceValue)
{
	D3D12_COMMAND_LIST_TYPE type = (D3D12_COMMAND_LIST_TYPE)(fenceValue >> 56);
	CommandQueue* pQueue = GetCommandQueue(type);
	pQueue->WaitForFence(fenceValue);
}

void Graphics::FreeCommandList(CommandContext* pCommandList)
{
	std::lock_guard<std::mutex> lockGuard(m_ContextAllocationMutex);
	m_FreeCommandLists[(int)pCommandList->GetType()].push(pCommandList);
}

D3D12_CPU_DESCRIPTOR_HANDLE Graphics::AllocateCpuDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	return m_DescriptorHeaps[type]->AllocateDescriptor();
}

void Graphics::IdleGPU()
{
	for (auto& pCommandQueue : m_CommandQueues)
	{
		if (pCommandQueue)
		{
			pCommandQueue->WaitForIdle();
		}
	}
}