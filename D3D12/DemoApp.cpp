#include "stdafx.h"
#include "DemoApp.h"
#include "Scene/Camera.h"
#include "ImGuizmo.h"
#include "Content/Image.h"
#include "Graphics/DebugRenderer.h"
#include "Graphics/Profiler.h"
#include "Graphics/Mesh.h"
#include "Graphics/RHI/Graphics.h"
#include "Graphics/RHI/Texture.h"
#include "Graphics/RHI/CommandContext.h"
#include "Graphics/RHI/DynamicResourceAllocator.h"
#include "Graphics/RHI/Shader.h"
#include "Graphics/RHI/PipelineState.h"
#include "Graphics/RHI/ShaderBindingTable.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/Techniques/GpuParticles.h"
#include "Graphics/Techniques/RTAO.h"
#include "Graphics/Techniques/TiledForward.h"
#include "Graphics/Techniques/ClusteredForward.h"
#include "Graphics/Techniques/RTReflections.h"
#include "Graphics/Techniques/PathTracing.h"
#include "Graphics/Techniques/SSAO.h"
#include "Graphics/Techniques/CBTTessellation.h"
#include "Graphics/Techniques/Clouds.h"
#include "Graphics/ImGuiRenderer.h"
#include "Core/TaskQueue.h"
#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/Input.h"
#include "Core/ConsoleVariables.h"
#include "Core/Utils.h"
#include "imgui_internal.h"
#include "IconsFontAwesome4.h"

static const DXGI_FORMAT DEPTH_STENCIL_SHADOW_FORMAT = DXGI_FORMAT_D32_FLOAT;

void EditTransform(const Camera& camera, Matrix& matrix)
{
	static ImGuizmo::OPERATION mCurrentGizmoOperation(ImGuizmo::ROTATE);
	static ImGuizmo::MODE mCurrentGizmoMode(ImGuizmo::WORLD);

	if (!Input::Instance().IsMouseDown(VK_LBUTTON))
	{
		if (Input::Instance().IsKeyPressed('W'))
			mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
		else if (Input::Instance().IsKeyPressed('E'))
			mCurrentGizmoOperation = ImGuizmo::ROTATE;
		else if (Input::Instance().IsKeyPressed('R'))
			mCurrentGizmoOperation = ImGuizmo::SCALE;
	}

	if (ImGui::RadioButton("Translate", mCurrentGizmoOperation == ImGuizmo::TRANSLATE))
		mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
	ImGui::SameLine();
	if (ImGui::RadioButton("Rotate", mCurrentGizmoOperation == ImGuizmo::ROTATE))
		mCurrentGizmoOperation = ImGuizmo::ROTATE;
	ImGui::SameLine();
	if (ImGui::RadioButton("Scale", mCurrentGizmoOperation == ImGuizmo::SCALE))
		mCurrentGizmoOperation = ImGuizmo::SCALE;
	float matrixTranslation[3], matrixRotation[3], matrixScale[3];
	ImGuizmo::DecomposeMatrixToComponents(&matrix.m[0][0], matrixTranslation, matrixRotation, matrixScale);
	ImGui::InputFloat3("Tr", matrixTranslation);
	ImGui::InputFloat3("Rt", matrixRotation);
	ImGui::InputFloat3("Sc", matrixScale);
	ImGuizmo::RecomposeMatrixFromComponents(matrixTranslation, matrixRotation, matrixScale, &matrix.m[0][0]);

	if (mCurrentGizmoOperation != ImGuizmo::SCALE)
	{
		if (ImGui::RadioButton("Local", mCurrentGizmoMode == ImGuizmo::LOCAL))
			mCurrentGizmoMode = ImGuizmo::LOCAL;
		ImGui::SameLine();
		if (ImGui::RadioButton("World", mCurrentGizmoMode == ImGuizmo::WORLD))
			mCurrentGizmoMode = ImGuizmo::WORLD;

		if (Input::Instance().IsKeyPressed(VK_SPACE))
		{
			mCurrentGizmoMode = mCurrentGizmoMode == ImGuizmo::LOCAL ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
		}
	}

	static Vector3 translationSnap = Vector3(1);
	static float rotateSnap = 5;
	static float scaleSnap = 0.1f;
	float* pSnapValue = &translationSnap.x;

	switch (mCurrentGizmoOperation)
	{
	case ImGuizmo::TRANSLATE:
		ImGui::InputFloat3("Snap", &translationSnap.x);
		pSnapValue = &translationSnap.x;
		break;
	case ImGuizmo::ROTATE:
		ImGui::InputFloat("Angle Snap", &rotateSnap);
		pSnapValue = &rotateSnap;
		break;
	case ImGuizmo::SCALE:
		ImGui::InputFloat("Scale Snap", &scaleSnap);
		pSnapValue = &scaleSnap;
		break;
	default:
		break;
	}

	Matrix view = camera.GetView();
	Matrix projection = camera.GetProjection();
	Math::ReverseZProjection(projection);
	ImGuizmo::Manipulate(&view.m[0][0], &projection.m[0][0], mCurrentGizmoOperation, mCurrentGizmoMode, &matrix.m[0][0], NULL, pSnapValue);
}

namespace Tweakables
{
	// Post processing
	ConsoleVariable g_WhitePoint("r.Exposure.WhitePoint", 1.0f);
	ConsoleVariable g_MinLogLuminance("r.Exposure.MinLogLuminance", -4.0f);
	ConsoleVariable g_MaxLogLuminance("r.Exposure.MaxLogLuminance", 20.0f);
	ConsoleVariable g_Tau("r.Exposure.Tau", 2.0f);
	ConsoleVariable g_DrawHistogram("vis.Histogram", false);
	ConsoleVariable g_ToneMapper("r.Tonemapper", 2);
	ConsoleVariable g_TAA("r.Taa", true);

	// Shadows
	ConsoleVariable g_SDSM("r.Shadows.SDSM", false);
	ConsoleVariable g_VisualizeShadowCascades("vis.ShadowCascades", false);
	ConsoleVariable g_ShadowCascades("r.Shadows.CascadeCount", 4);
	ConsoleVariable g_PSSMFactor("r.Shadow.PSSMFactor", 1.0f);

	// Bloom
	ConsoleVariable g_Bloom("r.Bloom", true);
	ConsoleVariable g_BloomThreshold("r.Bloom.Threshold", 4.0f);
	ConsoleVariable g_BloomMaxBrightness("r.Bloom.MaxBrightness", 8.0f);

	// Misc Lighting
	ConsoleVariable g_VolumetricFog("r.VolumetricFog", true);
	ConsoleVariable g_RaytracedAO("r.Raytracing.AO", false);
	ConsoleVariable g_VisualizeLights("vis.Lights", false);
	ConsoleVariable g_VisualizeLightDensity("vis.LightDensity", false);
	ConsoleVariable g_EnableDDGI("r.DDGI", true);
	ConsoleVariable g_VisualizeDDGI("vis.DDGI", false);
	ConsoleVariable g_RenderObjectBounds("r.vis.ObjectBounds", false);

	ConsoleVariable g_RaytracedReflections("r.Raytracing.Reflections", true);
	ConsoleVariable g_TLASBoundsThreshold("r.Raytracing.TLASBoundsThreshold", 1.0f * Math::DegreesToRadians);
	ConsoleVariable g_SsrSamples("r.SSRSamples", 8);
	ConsoleVariable g_RenderTerrain("r.Terrain", false);

	ConsoleVariable g_FreezeClusterCulling("r.FreezeClusterCulling", false);

	// Misc
	bool g_DumpRenderGraph = false;
	ConsoleCommand<> gDumpRenderGraph("DumpRenderGraph", []() { g_DumpRenderGraph = true; });
	bool g_Screenshot = false;
	ConsoleCommand<> gScreenshot("Screenshot", []() { g_Screenshot = true; });

	// Lighting
	float g_SunInclination = 0.79f;
	float g_SunOrientation = -1.503f;
	float g_SunTemperature = 5900.0f;
	float g_SunIntensity = 5.0f;
}

DemoApp::DemoApp(WindowHandle window, const IntVector2& windowRect)
	: m_Window(window)
{
	m_pCamera = std::make_unique<FreeCamera>();
	m_pCamera->SetNearPlane(80.0f);
	m_pCamera->SetFarPlane(0.1f);

	E_LOG(Info, "Graphics::InitD3D()");

	GraphicsDeviceOptions options;
	options.UseDebugDevice = CommandLine::GetBool("d3ddebug") || _DEBUG;
	options.UseDRED = CommandLine::GetBool("dred") || _DEBUG;
	options.LoadPIX = CommandLine::GetBool("pix");
	options.UseGPUValidation = CommandLine::GetBool("gpuvalidation");
	options.UseWarp = CommandLine::GetBool("warp");
	m_pDevice = new GraphicsDevice(options);
	m_pSwapchain = new SwapChain(m_pDevice, DisplayMode::SDR, window);

	GraphicsCommon::Create(m_pDevice);
	ImGuiRenderer::Initialize(m_pDevice, window);

	m_pClouds = std::make_unique<Clouds>(m_pDevice);
	m_pClusteredForward = std::make_unique<ClusteredForward>(m_pDevice);
	m_pTiledForward = std::make_unique<TiledForward>(m_pDevice);
	m_pRTReflections = std::make_unique<RTReflections>(m_pDevice);
	m_pRTAO = std::make_unique<RTAO>(m_pDevice);
	m_pSSAO = std::make_unique<SSAO>(m_pDevice);
	m_pParticles = std::make_unique<GpuParticles>(m_pDevice);
	m_pPathTracing = std::make_unique<PathTracing>(m_pDevice);
	m_pCBTTessellation = std::make_unique<CBTTessellation>(m_pDevice);

	Profiler::Get()->Initialize(m_pDevice);
	DebugRenderer::Get()->Initialize(m_pDevice);

	OnResizeOrMove(windowRect.x, windowRect.y);
	OnResizeViewport(windowRect.x, windowRect.y);

	CommandContext* pContext = m_pDevice->AllocateCommandContext();
	InitializePipelines();
	SetupScene(*pContext);
	pContext->Execute(true);

	m_RenderGraphPool = std::make_unique<RGResourcePool>(m_pDevice);

	Tweakables::g_RaytracedAO = m_pDevice->GetCapabilities().SupportsRaytracing() ? Tweakables::g_RaytracedAO : false;
	Tweakables::g_RaytracedReflections = m_pDevice->GetCapabilities().SupportsRaytracing() ? Tweakables::g_RaytracedReflections : false;

	if (m_RenderPath == RenderPath::Visibility && !m_pDevice->GetCapabilities().SupportsMeshShading())
		m_RenderPath = RenderPath::Clustered;
	else if (m_RenderPath == RenderPath::PathTracing && !m_pDevice->GetCapabilities().SupportsRaytracing())
		m_RenderPath = RenderPath::Clustered;
}

DemoApp::~DemoApp()
{
	m_pDevice->IdleGPU();
	ImGuiRenderer::Shutdown();
	GraphicsCommon::Destroy();
	DebugRenderer::Get()->Shutdown();
	Profiler::Get()->Shutdown();
}

void DemoApp::SetupScene(CommandContext& context)
{
	m_pCamera->SetPosition(Vector3(-1.3f, 2.4f, -1.5f));
	m_pCamera->SetRotation(Quaternion::CreateFromYawPitchRoll(Math::PIDIV4, Math::PIDIV4 * 0.5f, 0));

	{
#if 1
		m_pCamera->SetPosition(Vector3(-1.3f, 2.4f, -1.5f));
		m_pCamera->SetRotation(Quaternion::CreateFromYawPitchRoll(Math::PIDIV4, Math::PIDIV4 * 0.5f, 0));

		//LoadMesh("Resources/Scenes/Sponza/Sponza.gltf", context, m_World);
#elif 1
		m_pCamera->SetPosition(Vector3(-1.3f, 2.4f, -1.5f));
		m_pCamera->SetRotation(Quaternion::CreateFromYawPitchRoll(Math::PIDIV4, Math::PIDIV4 * 0.5f, 0));

		LoadMesh("C:/Users/simon.coenen/Downloads/Sponza_New/Processed/Main/NewSponza_Main_Blender_glTF.gltf", context, m_World);
		LoadMesh("C:/Users/simon.coenen/Downloads/Sponza_New/Processed/PKG_A_Curtains/NewSponza_Curtains_glTF.gltf", context, m_World);
		LoadMesh("C:/Users/simon.coenen/Downloads/Sponza_New/Processed/PKG_B_Ivy/NewSponza_IvyGrowth_glTF.gltf", context, m_World);
		LoadMesh("C:/Users/simon.coenen/Downloads/Sponza_New/Processed/PKG_D_Candles/NewSponza_100sOfCandles_glTF_OmniLights.gltf", context, m_World);
		//LoadMesh("C:/Users/simon.coenen/Downloads/Sponza_New/PKG_C_Trees/NewSponza_CypressTree_glTF.gltf", context);
#elif 0

		// Hardcode the camera of the scene :-)
		Matrix m(
			0.868393660f, 8.00937414e-08f, -0.495875478f, 0,
			0.0342082977f, 0.997617662f, 0.0599068627f, 0,
			0.494694114f, -0.0689857975f, 0.866324782f, 0,
			0, 0, 0, 1
		);

		m_pCamera->SetPosition(Vector3(-2.22535753f, 0.957680941f, -5.52742338f));
		m_pCamera->SetFoV(68.75f * Math::PI / 180.0f);
		m_pCamera->SetRotation(Quaternion::CreateFromRotationMatrix(m));

		LoadMesh("D:/References/GltfScenes/bathroom_pt/LAZIENKA.gltf", context);
#elif 1
		LoadMesh("D:/References/GltfScenes/Sphere/scene.gltf", context);
#elif 0
		LoadMesh("D:/References/GltfScenes/BlenderSplash/MyScene.gltf", context);
#endif
	}

	{
		Vector3 Position(-150, 160, -10);
		Vector3 Direction;
		Position.Normalize(Direction);
		Light sunLight = Light::Directional(Position, -Direction, 10);
		sunLight.CastShadows = true;
		sunLight.VolumetricLighting = true;
		m_World.Lights.push_back(sunLight);
	}

#if 1
	{
		Vector3 Position(9, 1.5f, 0);
		Light pointLights = Light::Point(Position, 5, 10, Colors::White);
		pointLights.CastShadows = true;
		pointLights.VolumetricLighting = true;
		m_World.Lights.push_back(pointLights);
	}
#endif

#if 0
	for (int i = 0; i < 5; ++i)
	{
		Vector3 loc(
			Math::RandomRange(-10.0f, 10.0f),
			Math::RandomRange(-4.0f, 5.0f),
			Math::RandomRange(-10.0f, 10.0f)
		);
		Light spotLight = Light::Spot(loc, 100, Vector3(0, 1, 0), 65, 50, 1000, Color(Math::RandomRange(0.0f, 1.0f), Math::RandomRange(0.0f, 1.0f), Math::RandomRange(0.0f, 1.0f), 1.0f));
		spotLight.CastShadows = true;
		//spotLight.LightTexture = m_pDevice->RegisterBindlessResource(m_pLightCookie.get(), GetDefaultTexture(DefaultTexture::White2D));
		spotLight.VolumetricLighting = true;
		m_Lights.push_back(spotLight);
	}
#endif

	DDGIVolume volume;
	volume.Origin = Vector3(-0.484151840f, 5.21196413f, 0.309524536f);
	volume.Extents = Vector3(14.8834171f, 6.22350454f, 9.15293312f);
	volume.NumProbes = IntVector3(16, 12, 14);
	volume.NumRays = 128;
	volume.MaxNumRays = 512;
	m_World.DDGIVolumes.push_back(volume);
}

void DemoApp::Update()
{
	PROFILE_BEGIN("Update");
	ImGuiRenderer::NewFrame();
	m_pDevice->GetShaderManager()->ConditionallyReloadShaders();
	UpdateImGui();
	m_pCamera->Update();

	m_RenderGraphPool->Tick();

	if (Input::Instance().IsKeyPressed('1'))
	{
		m_RenderPath = RenderPath::Clustered;
	}
	else if (Input::Instance().IsKeyPressed('2'))
	{
		m_RenderPath = RenderPath::Tiled;
	}
	else if (Input::Instance().IsKeyPressed('3') && m_pVisibilityRenderingPSO)
	{
		m_RenderPath = RenderPath::Visibility;
	}
	else if (Input::Instance().IsKeyPressed('4') && m_pPathTracing->IsSupported())
	{
		m_RenderPath = RenderPath::PathTracing;
	}

	if (Tweakables::g_RenderObjectBounds)
	{
		for (const Batch& b : m_SceneData.Batches)
		{
			DebugRenderer::Get()->AddBoundingBox(b.Bounds, Color(0.2f, 0.2f, 0.9f, 1.0f));
			DebugRenderer::Get()->AddSphere(b.Bounds.Center, b.Radius, 6, 6, Color(0.2f, 0.6f, 0.2f, 1.0f));
		}
	}

	float costheta = cosf(Tweakables::g_SunOrientation);
	float sintheta = sinf(Tweakables::g_SunOrientation);
	float cosphi = cosf(Tweakables::g_SunInclination * Math::PIDIV2);
	float sinphi = sinf(Tweakables::g_SunInclination * Math::PIDIV2);
	m_World.Lights[0].Direction = -Vector3(costheta * cosphi, sinphi, sintheta * cosphi);
	m_World.Lights[0].Colour = Math::MakeFromColorTemperature(Tweakables::g_SunTemperature);
	m_World.Lights[0].Intensity = Tweakables::g_SunIntensity;

	if (m_World.DDGIVolumes.size() > 0)
	{
		DDGIVolume& volume = m_World.DDGIVolumes[0];
		volume.Origin = m_SceneData.SceneAABB.Center;
		volume.Extents = 1.1f * Vector3(m_SceneData.SceneAABB.Extents);
	}

	if (Tweakables::g_VisualizeLights)
	{
		for (const Light& light : m_World.Lights)
		{
			DebugRenderer::Get()->AddLight(light);
		}
	}

	CreateShadowViews(m_SceneData, m_World);
	m_SceneData.View = m_pCamera->GetViewTransform();
	m_SceneData.FrameIndex = m_Frame;

	{
		PROFILE_SCOPE("Frustum Culling");
		bool boundsSet = false;
		BoundingFrustum frustum = m_pCamera->GetFrustum();
		for (const Batch& b : m_SceneData.Batches)
		{
			m_SceneData.VisibilityMask.AssignBit(b.InstanceData.World, frustum.Contains(b.Bounds));
			if (boundsSet)
			{
				BoundingBox::CreateMerged(m_SceneData.SceneAABB, m_SceneData.SceneAABB, b.Bounds);
			}
			else
			{
				m_SceneData.SceneAABB = b.Bounds;
				boundsSet = true;
			}
		}
	}

	////////////////////////////////
	// LET THE RENDERING BEGIN!
	////////////////////////////////

	if (Tweakables::g_Screenshot)
	{
		Tweakables::g_Screenshot = false;

		CommandContext* pContext = m_pDevice->AllocateCommandContext();
		Texture* pSource = m_ColorOutput;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT textureFootprint = {};
		D3D12_RESOURCE_DESC resourceDesc = m_ColorOutput->GetResource()->GetDesc();
		m_pDevice->GetDevice()->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &textureFootprint, nullptr, nullptr, nullptr);
		RefCountPtr<Buffer> pScreenshotBuffer = m_pDevice->CreateBuffer(BufferDesc::CreateReadback(textureFootprint.Footprint.RowPitch * textureFootprint.Footprint.Height), "Screenshot Texture");
		pContext->InsertResourceBarrier(m_ColorOutput, D3D12_RESOURCE_STATE_COPY_SOURCE);
		pContext->InsertResourceBarrier(pScreenshotBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
		pContext->CopyTexture(m_ColorOutput, pScreenshotBuffer, CD3DX12_BOX(0, 0, m_ColorOutput->GetWidth(), m_ColorOutput->GetHeight()));

		ScreenshotRequest request;
		request.Width = pSource->GetWidth();
		request.Height = pSource->GetHeight();
		request.RowPitch = textureFootprint.Footprint.RowPitch;
		request.pBuffer = pScreenshotBuffer;
		request.SyncPoint = pContext->Execute(false);
		m_ScreenshotBuffers.emplace(request);
	}

	if (!m_ScreenshotBuffers.empty())
	{
		while (!m_ScreenshotBuffers.empty() && m_ScreenshotBuffers.front().SyncPoint.IsComplete())
		{
			const ScreenshotRequest& request = m_ScreenshotBuffers.front();

			TaskContext taskContext;
			TaskQueue::Execute([request](uint32)
				{
					char* pData = (char*)request.pBuffer->GetMappedData();
					Image img;
					img.SetSize(request.Width, request.Height, 4);
					uint32 imageRowPitch = request.Width * 4;
					uint32 targetOffset = 0;
					for (uint32 i = 0; i < request.Height; ++i)
					{
						img.SetData((uint32*)pData, targetOffset, imageRowPitch);
						pData += request.RowPitch;
						targetOffset += imageRowPitch;
					}

					SYSTEMTIME time;
					GetSystemTime(&time);
					Paths::CreateDirectoryTree(Paths::ScreenshotDir());
					img.Save(Sprintf("%sScreenshot_%s", Paths::ScreenshotDir().c_str(), Utils::GetTimeString().c_str()).c_str());
				}, taskContext);
			m_ScreenshotBuffers.pop();
		}
	}

	const SceneView* pView = &m_SceneData;
	//const World* pWorld = &m_World;
	SceneView* pViewMut = &m_SceneData;
	World* pWorldMut = &m_World;

	{
		CommandContext* pContext = m_pDevice->AllocateCommandContext();
		Renderer::UploadSceneData(*pContext, pViewMut, pWorldMut);
		pContext->Execute(false);
	}

	RGGraph graph(m_pDevice, *m_RenderGraphPool);
	SceneTextures sceneTextures;

	IntVector2 viewDimensions = m_SceneData.GetDimensions();
	sceneTextures.pPreviousColor =		RGUtils::CreatePersistentTexture(graph, "Color History", TextureDesc::CreateRenderTarget(viewDimensions.x, viewDimensions.y, DXGI_FORMAT_R16G16B16A16_FLOAT), &m_pColorHistory, true);
	sceneTextures.pVisibilityBuffer =	graph.CreateTexture("Visibility Buffer",	TextureDesc::CreateRenderTarget(viewDimensions.x, viewDimensions.y, DXGI_FORMAT_R32_UINT));
	sceneTextures.pRoughness =			graph.CreateTexture("Roughness",			TextureDesc::CreateRenderTarget(viewDimensions.x, viewDimensions.y, DXGI_FORMAT_R8_UNORM));
	sceneTextures.pColorTarget =		graph.CreateTexture("Color Target",			TextureDesc::CreateRenderTarget(viewDimensions.x, viewDimensions.y, DXGI_FORMAT_R16G16B16A16_FLOAT));
	sceneTextures.pAmbientOcclusion =	graph.CreateTexture("Ambient Occlusion",	TextureDesc::Create2D(viewDimensions.x, viewDimensions.y, DXGI_FORMAT_R8_UNORM));
	sceneTextures.pNormals =			graph.CreateTexture("Normals",				TextureDesc::CreateRenderTarget(viewDimensions.x, viewDimensions.y, DXGI_FORMAT_R16G16_FLOAT));
	sceneTextures.pVelocity =			graph.CreateTexture("Velocity",				TextureDesc::CreateRenderTarget(viewDimensions.x, viewDimensions.y, DXGI_FORMAT_R16G16_FLOAT));
	sceneTextures.pDepth =				graph.CreateTexture("Depth Stencil",		TextureDesc::CreateDepth(viewDimensions.x, viewDimensions.y, DXGI_FORMAT_D32_FLOAT, TextureFlag::None, 1, ClearBinding(0.0f, 0)));
	sceneTextures.pResolvedDepth =		graph.CreateTexture("Resolved Depth",		TextureDesc::CreateDepth(viewDimensions.x, viewDimensions.y, DXGI_FORMAT_D32_FLOAT, TextureFlag::None, 1, ClearBinding(0.0f, 0)));

	if (m_RenderPath == RenderPath::Clustered || m_RenderPath == RenderPath::Tiled || m_RenderPath == RenderPath::Visibility)
	{
		RG_GRAPH_SCOPE("Shadow Depths", graph);
		for (uint32 i = 0; i < (uint32)pView->ShadowViews.size(); ++i)
		{
			graph.AddPass(Sprintf("View %d", i).c_str(), RGPassFlag::Raster | RGPassFlag::NeverCull)
				.Bind([=](CommandContext& context, const RGPassResources& /*resources*/)
					{
						context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
						context.SetGraphicsRootSignature(m_pCommonRS);

						// hack - copy the main viewport and then just modify the viewproj
						SceneView view = *pView;

						const ShadowView& shadowView = view.ShadowViews[i];
						Texture* pShadowmap = shadowView.pDepthTexture;
						context.InsertResourceBarrier(pShadowmap, D3D12_RESOURCE_STATE_DEPTH_WRITE);
						context.BeginRenderPass(RenderPassInfo::DepthOnly(pShadowmap, RenderPassAccess::Clear_Store));

						view.View.ViewProjection = shadowView.ViewProjection;
						context.SetRootCBV(1, Renderer::GetViewUniforms(&view, pShadowmap));

						{
							GPU_PROFILE_SCOPE("Opaque", &context);
							context.SetPipelineState(m_pShadowsOpaquePSO);
							Renderer::DrawScene(context, &view, shadowView.Visibility, Batch::Blending::Opaque);
						}
						{
							GPU_PROFILE_SCOPE("Masked", &context);
							context.SetPipelineState(m_pShadowsAlphaMaskPSO);
							Renderer::DrawScene(context, &view, shadowView.Visibility, Batch::Blending::AlphaMask | Batch::Blending::AlphaBlend);
						}
						context.EndRenderPass();
					});
		}
	}

	if (m_RenderPath == RenderPath::Clustered || m_RenderPath == RenderPath::Tiled)
	{
		// - Depth only pass that renders the entire scene
		// - Optimization that prevents wasteful lighting calculations during the base pass
		// - Required for light culling
		graph.AddPass("Depth Prepass", RGPassFlag::Raster)
			.DepthStencil(sceneTextures.pDepth, RenderTargetLoadAction::Clear, true)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					context.BeginRenderPass(resources.GetRenderPassInfo());
					context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

					context.SetGraphicsRootSignature(m_pCommonRS);

					context.SetRootCBV(1, Renderer::GetViewUniforms(pView, sceneTextures.pDepth->Get()));

					{
						GPU_PROFILE_SCOPE("Opaque", &context);
						context.SetPipelineState(m_pDepthPrepassOpaquePSO);
						Renderer::DrawScene(context, pView, Batch::Blending::Opaque);
					}
					{
						GPU_PROFILE_SCOPE("Masked", &context);
						context.SetPipelineState(m_pDepthPrepassAlphaMaskPSO);
						Renderer::DrawScene(context, pView, Batch::Blending::AlphaMask);
					}

					context.EndRenderPass();
				});
	}
	else if (m_RenderPath == RenderPath::Visibility)
	{
		graph.AddPass("Visibility Buffer", RGPassFlag::Raster)
			.DepthStencil(sceneTextures.pDepth, RenderTargetLoadAction::Clear, true)
			.RenderTarget(sceneTextures.pVisibilityBuffer, RenderTargetLoadAction::DontCare)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					context.BeginRenderPass(resources.GetRenderPassInfo());
					context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

					context.SetGraphicsRootSignature(m_pCommonRS);

					context.SetRootCBV(1, Renderer::GetViewUniforms(pView, sceneTextures.pVisibilityBuffer->Get()));
					{
						GPU_PROFILE_SCOPE("Opaque", &context);
						context.SetPipelineState(m_pVisibilityRenderingPSO);
						Renderer::DrawScene(context, pView, Batch::Blending::Opaque);
					}

					{
						GPU_PROFILE_SCOPE("Opaque Masked", &context);
						context.SetPipelineState(m_pVisibilityRenderingMaskedPSO);
						Renderer::DrawScene(context, pView, Batch::Blending::AlphaMask);
					}

					context.EndRenderPass();
				});
	}

	if (m_RenderPath == RenderPath::Clustered || m_RenderPath == RenderPath::Tiled || m_RenderPath == RenderPath::Visibility)
	{
		m_pParticles->Simulate(graph, pView, sceneTextures.pDepth);
	}

	if ((m_RenderPath == RenderPath::Clustered || m_RenderPath == RenderPath::Tiled || m_RenderPath == RenderPath::Visibility) && Tweakables::g_EnableDDGI && m_World.DDGIVolumes.size() > 0 && m_pDevice->GetCapabilities().SupportsRaytracing())
	{
		RG_GRAPH_SCOPE("DDGI", graph);

		uint32 randomIndex = Math::RandomRange(0, (int)m_World.DDGIVolumes.size() - 1);
		DDGIVolume& ddgi = m_World.DDGIVolumes[randomIndex];

		struct
		{
			Vector3 RandomVector;
			float RandomAngle;
			float HistoryBlendWeight;
			uint32 VolumeIndex;
		} parameters;

		parameters.RandomVector = Math::RandVector();
		parameters.RandomAngle = Math::RandomRange(0.0f, 2.0f * Math::PI);
		parameters.HistoryBlendWeight = 0.98f;
		parameters.VolumeIndex = randomIndex;

		const uint32 numProbes = ddgi.NumProbes.x * ddgi.NumProbes.y * ddgi.NumProbes.z;

		// Must match with shader!
		constexpr uint32 probeIrradianceTexels = 6;
		constexpr uint32 probeDepthTexel = 14;
		auto ProbeTextureDimensions = [](const IntVector3& numProbes, uint32 texelsPerProbe) {
			uint32 width = (1 + texelsPerProbe + 1) * numProbes.y * numProbes.x;
			uint32 height = (1 + texelsPerProbe + 1) * numProbes.z;
			return IntVector2(width, height);
		};

		IntVector2 ddgiIrradianceDimensions = ProbeTextureDimensions(ddgi.NumProbes, probeIrradianceTexels);
		TextureDesc ddgiIrradianceDesc = TextureDesc::Create2D(ddgiIrradianceDimensions.x, ddgiIrradianceDimensions.y, DXGI_FORMAT_R16G16B16A16_FLOAT, TextureFlag::UnorderedAccess);
		RGTexture* pIrradianceTarget = graph.CreateTexture("DDGI Irradiance Target", ddgiIrradianceDesc);
		RGTexture* pIrradianceHistory = RGUtils::CreatePersistentTexture(graph, "DDGI Irradiance History", ddgiIrradianceDesc, &ddgi.pIrradianceHistory, false);
		graph.ExportTexture(pIrradianceTarget, &ddgi.pIrradianceHistory);

		IntVector2 ddgiDepthDimensions = ProbeTextureDimensions(ddgi.NumProbes, probeDepthTexel);
		TextureDesc ddgiDepthDesc = TextureDesc::Create2D(ddgiDepthDimensions.x, ddgiDepthDimensions.y, DXGI_FORMAT_R16G16_FLOAT, TextureFlag::UnorderedAccess);
		RGTexture* pDepthTarget = graph.CreateTexture("DDGI Depth Target", ddgiDepthDesc);
		RGTexture* pDepthHistory = RGUtils::CreatePersistentTexture(graph, "DDGI Depth History", ddgiDepthDesc, &ddgi.pDepthHistory, false);
		graph.ExportTexture(pDepthTarget, &ddgi.pDepthHistory);

		RGBuffer* pRayBuffer = graph.CreateBuffer("DDGI Ray Buffer", BufferDesc::CreateTyped(numProbes * ddgi.MaxNumRays, DXGI_FORMAT_R16G16B16A16_FLOAT));
		RGBuffer* pProbeStates = RGUtils::CreatePersistentBuffer(graph, "DDGI States Buffer", BufferDesc::CreateTyped(numProbes, DXGI_FORMAT_R8_UINT), &ddgi.pProbeStates, true);
		RGBuffer* pProbeOffsets = RGUtils::CreatePersistentBuffer(graph, "DDGI Probe Offsets", BufferDesc::CreateTyped(numProbes, DXGI_FORMAT_R16G16B16A16_FLOAT), &ddgi.pProbeOffset, true);

		graph.AddPass("DDGI Raytrace", RGPassFlag::Compute)
			.Read(pProbeStates)
			.Write(pRayBuffer)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pDDGITraceRaysSO);

					context.SetRootConstants(0, parameters);
					context.SetRootCBV(1, Renderer::GetViewUniforms(pView));
					context.BindResources(2, pRayBuffer->Get()->GetUAV());

					ShaderBindingTable bindingTable(m_pDDGITraceRaysSO);
					bindingTable.BindRayGenShader("TraceRaysRGS");
					bindingTable.BindMissShader("MaterialMS", 0);
					bindingTable.BindMissShader("OcclusionMS", 1);
					bindingTable.BindHitGroup("MaterialHG", 0);

					context.DispatchRays(bindingTable, ddgi.NumRays, numProbes);
				});

		graph.AddPass("DDGI Update Irradiance", RGPassFlag::Compute)
			.Read({ pIrradianceHistory, pRayBuffer, pProbeStates })
			.Write(pIrradianceTarget)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pDDGIUpdateIrradianceColorPSO);

					context.SetRootConstants(0, parameters);
					context.SetRootCBV(1, Renderer::GetViewUniforms(pView));
					context.BindResources(2, pIrradianceTarget->Get()->GetUAV());
					context.BindResources(3, {
						pRayBuffer->Get()->GetSRV(),
						});

					context.Dispatch(numProbes);
				});

		graph.AddPass("DDGI Update Depth", RGPassFlag::Compute)
			.Read({ pDepthHistory, pRayBuffer, pProbeStates })
			.Write(pDepthTarget)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pDDGIUpdateIrradianceDepthPSO);

					context.SetRootConstants(0, parameters);
					context.SetRootCBV(1, Renderer::GetViewUniforms(pView));
					context.BindResources(2, {
						pDepthTarget->Get()->GetUAV(),
						});
					context.BindResources(3, {
						pRayBuffer->Get()->GetSRV(),
						});

					context.Dispatch(numProbes);
				});

		graph.AddPass("DDGI Update Probe States", RGPassFlag::Compute)
			.Read(pRayBuffer)
			.Write({ pProbeOffsets, pProbeStates })
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pDDGIUpdateProbeStatesPSO);

					context.SetRootConstants(0, parameters);
					context.SetRootCBV(1, Renderer::GetViewUniforms(pView));
					context.BindResources(2, {
						pProbeStates->Get()->GetUAV(),
						pProbeOffsets->Get()->GetUAV(),
						});
					context.BindResources(3, {
						pRayBuffer->Get()->GetSRV(),
						});

					context.Dispatch(ComputeUtils::GetNumThreadGroups(numProbes, 32));
				});

		graph.AddPass("Bindless Transition", RGPassFlag::NeverCull | RGPassFlag::Raster)
			.Read({ pDepthTarget, pIrradianceTarget, pProbeStates, pProbeOffsets });
	}

	RGTexture* pSky = graph.CreateTexture("Sky", TextureDesc::Create2D(64, 128, DXGI_FORMAT_R16G16B16A16_FLOAT, TextureFlag::ShaderResource | TextureFlag::UnorderedAccess));
	graph.ExportTexture(pSky, &pViewMut->pSky);

	graph.AddPass("Compute Sky", RGPassFlag::Compute | RGPassFlag::NeverCull)
		.Write(pSky)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
			{
				Texture* pSkyTexture = pSky->Get();
				context.SetComputeRootSignature(m_pCommonRS);
				context.SetPipelineState(m_pRenderSkyPSO);

				context.SetRootCBV(1, Renderer::GetViewUniforms(pView, pSkyTexture));
				context.BindResources(2, pSkyTexture->GetUAV());

				context.Dispatch(ComputeUtils::GetNumThreadGroups(pSkyTexture->GetWidth(), 16, pSkyTexture->GetHeight(), 16));

				context.InsertResourceBarrier(pSkyTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			});

	if (m_RenderPath == RenderPath::Clustered || m_RenderPath == RenderPath::Tiled || m_RenderPath == RenderPath::Visibility)
	{
		//[WITH MSAA] DEPTH RESOLVE
		// - If MSAA is enabled, run a compute shader to resolve the depth buffer
		if (sceneTextures.pDepth->GetDesc().SampleCount > 1)
		{
			graph.AddPass("Depth Resolve", RGPassFlag::Compute)
				.Read(sceneTextures.pDepth)
				.Write(sceneTextures.pResolvedDepth)
				.Bind([=](CommandContext& context, const RGPassResources& resources)
					{
						Texture* pResolveTarget = sceneTextures.pResolvedDepth->Get();
						context.SetComputeRootSignature(m_pCommonRS);
						context.SetPipelineState(m_pResolveDepthPSO);

						context.BindResources(2, pResolveTarget->GetUAV());
						context.BindResources(3, sceneTextures.pDepth->Get()->GetSRV());

						context.Dispatch(ComputeUtils::GetNumThreadGroups(pResolveTarget->GetWidth(), 16, pResolveTarget->GetHeight(), 16));
					});
		}
		else
		{
			sceneTextures.pResolvedDepth = sceneTextures.pDepth;
		}

		graph.AddPass("Camera Motion", RGPassFlag::Compute)
			.Read(sceneTextures.pDepth)
			.Write(sceneTextures.pVelocity)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					Texture* pVelocity = sceneTextures.pVelocity->Get();

					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pCameraMotionPSO);

					context.SetRootCBV(1, Renderer::GetViewUniforms(pView, pVelocity));
					context.BindResources(2, pVelocity->GetUAV());
					context.BindResources(3, sceneTextures.pDepth->Get()->GetSRV());

					context.Dispatch(ComputeUtils::GetNumThreadGroups(pVelocity->GetWidth(), 8, pVelocity->GetHeight(), 8));
				});

		if (Tweakables::g_RaytracedAO)
		{
			m_pRTAO->Execute(graph, pView, sceneTextures);
		}
		else
		{
			m_pSSAO->Execute(graph, pView, sceneTextures);
		}

		if (m_RenderPath == RenderPath::Tiled)
		{
			m_pTiledForward->Execute(graph, pView, sceneTextures);
		}
		else if (m_RenderPath == RenderPath::Clustered)
		{
			m_pClusteredForward->Execute(graph, pView, sceneTextures);
		}
		else if (m_RenderPath == RenderPath::Visibility)
		{
			graph.AddPass("Visibility Shading", RGPassFlag::Compute)
				.Read({ sceneTextures.pVisibilityBuffer, sceneTextures.pDepth, sceneTextures.pAmbientOcclusion, sceneTextures.pPreviousColor })
				.Write({ sceneTextures.pNormals, sceneTextures.pColorTarget, sceneTextures.pRoughness })
				.Bind([=](CommandContext& context, const RGPassResources& resources)
					{
						Texture* pColorTarget = sceneTextures.pColorTarget->Get();

						context.SetComputeRootSignature(m_pCommonRS);
						context.SetPipelineState(m_pVisibilityShadingPSO);

						context.SetRootCBV(1, Renderer::GetViewUniforms(pView, pColorTarget));
						context.BindResources(2, {
							pColorTarget->GetUAV(),
							sceneTextures.pNormals->Get()->GetUAV(),
							sceneTextures.pRoughness->Get()->GetUAV(),
							});
						context.BindResources(3, {
							sceneTextures.pVisibilityBuffer->Get()->GetSRV(),
							sceneTextures.pAmbientOcclusion->Get()->GetSRV(),
							sceneTextures.pDepth->Get()->GetSRV(),
							sceneTextures.pPreviousColor->Get()->GetSRV(),
							});
						context.Dispatch(ComputeUtils::GetNumThreadGroups(pColorTarget->GetWidth(), 8, pColorTarget->GetHeight(), 8));
					});
		}

		m_pParticles->Render(graph, pView, sceneTextures);

		if (Tweakables::g_RenderTerrain.GetBool())
		{
			m_pCBTTessellation->Execute(graph, pView, sceneTextures);
		}

		graph.AddPass("Render Sky", RGPassFlag::Raster)
			.Read(pSky)
			.DepthStencil(sceneTextures.pDepth, RenderTargetLoadAction::Load, false)
			.RenderTarget(sceneTextures.pColorTarget, RenderTargetLoadAction::Load)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					context.BeginRenderPass(resources.GetRenderPassInfo());
					context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					context.SetGraphicsRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pSkyboxPSO);

					context.SetRootCBV(1, Renderer::GetViewUniforms(pView, sceneTextures.pColorTarget->Get()));
					context.Draw(0, 36);

					context.EndRenderPass();
				});

		RGTexture* pTex = m_pClouds->Render(graph, sceneTextures, pView);
		VisualizeTexture(graph, pTex);

		DebugRenderer::Get()->Render(graph, pView, sceneTextures.pColorTarget, sceneTextures.pDepth);
	}
	else if (m_RenderPath == RenderPath::PathTracing)
	{
		m_pPathTracing->Render(graph, pView, sceneTextures.pColorTarget);
	}

	TextureDesc colorDesc = sceneTextures.pColorTarget->GetDesc();
	if (colorDesc.SampleCount > 1)
	{
		colorDesc.SampleCount = 1;
		RGTexture* pResolveColor = graph.CreateTexture("Resolved Color", colorDesc);
		graph.AddPass("Color Resolve", RGPassFlag::Compute)
			.RenderTarget(sceneTextures.pColorTarget, RenderTargetLoadAction::Load, pResolveColor)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					context.BeginRenderPass(resources.GetRenderPassInfo());
					context.EndRenderPass();
				});
		sceneTextures.pColorTarget = pResolveColor;
	}

	if (m_RenderPath != RenderPath::PathTracing)
	{
		if (Tweakables::g_RaytracedReflections)
		{
			m_pRTReflections->Execute(graph, pView, sceneTextures);
		}

		if (Tweakables::g_TAA.Get())
		{
			RGTexture* pTaaTarget = graph.CreateTexture("TAA Target", sceneTextures.pColorTarget->GetDesc());

			graph.AddPass("Temporal Resolve", RGPassFlag::Compute)
				.Read({ sceneTextures.pVelocity, sceneTextures.pDepth, sceneTextures.pColorTarget, sceneTextures.pPreviousColor })
				.Write(pTaaTarget)
				.Bind([=](CommandContext& context, const RGPassResources& resources)
					{
						Texture* pTarget = pTaaTarget->Get();
						context.SetComputeRootSignature(m_pCommonRS);
						context.SetPipelineState(m_pTemporalResolvePSO);

						context.SetRootCBV(1, Renderer::GetViewUniforms(pView, pTarget));
						context.BindResources(2, pTarget->GetUAV());
						context.BindResources(3,
							{
								sceneTextures.pVelocity->Get()->GetSRV(),
								sceneTextures.pPreviousColor->Get()->GetSRV(),
								sceneTextures.pColorTarget->Get()->GetSRV(),
								sceneTextures.pDepth->Get()->GetSRV(),
							});

						context.Dispatch(ComputeUtils::GetNumThreadGroups(pTarget->GetWidth(), 8, pTarget->GetHeight(), 8));
					});

			RGUtils::AddCopyPass(graph, pTaaTarget, sceneTextures.pPreviousColor);

			sceneTextures.pColorTarget = pTaaTarget;
		}
	}

	if (Tweakables::g_SDSM)
	{
		RG_GRAPH_SCOPE("Depth Reduce", graph);

		IntVector3 depthSize = sceneTextures.pDepth->GetDesc().Size();
		depthSize.x = Math::DivideAndRoundUp(depthSize.x, 16);
		depthSize.y = Math::DivideAndRoundUp(depthSize.y, 16);
		RGTexture* pReductionTarget = graph.CreateTexture("Depth Reduction Target", TextureDesc::Create2D(depthSize.x, depthSize.y, DXGI_FORMAT_R32G32_FLOAT));

		graph.AddPass("Depth Reduce - Setup", RGPassFlag::Compute)
			.Read(sceneTextures.pDepth)
			.Write(pReductionTarget)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					Texture* pSource = sceneTextures.pDepth->Get();
					Texture* pTarget = pReductionTarget->Get();

					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(pSource->GetDesc().SampleCount > 1 ? m_pPrepareReduceDepthMsaaPSO : m_pPrepareReduceDepthPSO);

					context.SetRootCBV(1, Renderer::GetViewUniforms(pView, pTarget));
					context.BindResources(2, pTarget->GetUAV());
					context.BindResources(3, pSource->GetSRV());

					context.Dispatch(pTarget->GetWidth(), pTarget->GetHeight());
				});

		while (depthSize.x > 1 || depthSize.y > 1)
		{
			RGTexture* pReductionSource = pReductionTarget;
			pReductionTarget = graph.CreateTexture("Depth Reduction Target", TextureDesc::Create2D(depthSize.x, depthSize.y, DXGI_FORMAT_R32G32_FLOAT));

			graph.AddPass("Depth Reduce - Subpass", RGPassFlag::Compute)
				.Read(pReductionSource)
				.Write(pReductionTarget)
				.Bind([=](CommandContext& context, const RGPassResources& resources)
					{
						Texture* pTarget = pReductionTarget->Get();
						context.SetPipelineState(m_pReduceDepthPSO);
						context.BindResources(2, pTarget->GetUAV());
						context.BindResources(3, pReductionSource->Get()->GetSRV());
						context.Dispatch(pTarget->GetWidth(), pTarget->GetHeight());
					});

			depthSize.x = Math::DivideAndRoundUp(depthSize.x, 16);
			depthSize.y = Math::DivideAndRoundUp(depthSize.y, 16);
		}

		graph.AddPass("Readback Copy", RGPassFlag::Copy)
			.Read(pReductionTarget)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					context.CopyTexture(pReductionTarget->Get(), m_ReductionReadbackTargets[m_Frame % SwapChain::NUM_FRAMES], CD3DX12_BOX(0, 1));
				});
	}

	RGBuffer* pLuminanceHistogram = graph.CreateBuffer("Luminance Histogram", BufferDesc::CreateByteAddress(sizeof(uint32) * 256));
	RGBuffer* pAverageLuminance = graph.TryImportBuffer(m_pAverageLuminance);
	if (!pAverageLuminance)
		pAverageLuminance = graph.CreateBuffer("Average Luminance", BufferDesc::CreateStructured(3, sizeof(float)));

	{
		RG_GRAPH_SCOPE("Eye Adaptation", graph);

		TextureDesc sourceDesc = sceneTextures.pColorTarget->GetDesc();
		sourceDesc.Width = Math::DivideAndRoundUp(sourceDesc.Width, 4);
		sourceDesc.Height = Math::DivideAndRoundUp(sourceDesc.Height, 4);
		RGTexture* pDownscaleTarget = graph.CreateTexture("Downscaled HDR Target", sourceDesc);

		graph.AddPass("Downsample Color", RGPassFlag::Compute)
			.Read(sceneTextures.pColorTarget)
			.Write(pDownscaleTarget)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					Texture* pTarget = pDownscaleTarget->Get();

					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pGenerateMipsPSO);

					struct
					{
						IntVector2 TargetDimensions;
						Vector2 TargetDimensionsInv;
					} parameters;
					parameters.TargetDimensions.x = pTarget->GetWidth();
					parameters.TargetDimensions.y = pTarget->GetHeight();
					parameters.TargetDimensionsInv = Vector2(1.0f / pTarget->GetWidth(), 1.0f / pTarget->GetHeight());

					context.SetRootConstants(0, parameters);
					context.BindResources(2, pTarget->GetUAV());
					context.BindResources(3, sceneTextures.pColorTarget->Get()->GetSRV());

					context.Dispatch(ComputeUtils::GetNumThreadGroups(parameters.TargetDimensions.x, 8, parameters.TargetDimensions.y, 8));
				});

		graph.AddPass("Luminance Histogram", RGPassFlag::Compute)
			.Read(pDownscaleTarget)
			.Write(pLuminanceHistogram)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					Texture* pColorSource = pDownscaleTarget->Get();
					Buffer* pHistogram = pLuminanceHistogram->Get();

					context.ClearUavUInt(pHistogram, pHistogram->GetUAV());

					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pLuminanceHistogramPSO);

					struct
					{
						uint32 Width;
						uint32 Height;
						float MinLogLuminance;
						float OneOverLogLuminanceRange;
					} parameters;
					parameters.Width = pColorSource->GetWidth();
					parameters.Height = pColorSource->GetHeight();
					parameters.MinLogLuminance = Tweakables::g_MinLogLuminance.Get();
					parameters.OneOverLogLuminanceRange = 1.0f / (Tweakables::g_MaxLogLuminance.Get() - Tweakables::g_MinLogLuminance.Get());

					context.SetRootConstants(0, parameters);
					context.BindResources(2, pHistogram->GetUAV());
					context.BindResources(3, pColorSource->GetSRV());

					context.Dispatch(ComputeUtils::GetNumThreadGroups(pColorSource->GetWidth(), 16, pColorSource->GetHeight(), 16));
				});

		uint32 numPixels = sourceDesc.Width * sourceDesc.Height;

		graph.AddPass("Average Luminance", RGPassFlag::Compute)
			.Read(pLuminanceHistogram)
			.Write(pAverageLuminance)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pAverageLuminancePSO);

					struct
					{
						int32 PixelCount;
						float MinLogLuminance;
						float LogLuminanceRange;
						float TimeDelta;
						float Tau;
					} parameters;

					parameters.PixelCount = numPixels;
					parameters.MinLogLuminance = Tweakables::g_MinLogLuminance.Get();
					parameters.LogLuminanceRange = Tweakables::g_MaxLogLuminance.Get() - Tweakables::g_MinLogLuminance.Get();
					parameters.TimeDelta = Time::DeltaTime();
					parameters.Tau = Tweakables::g_Tau.Get();

					context.SetRootConstants(0, parameters);
					context.BindResources(2, pAverageLuminance->Get()->GetUAV());
					context.BindResources(3, pLuminanceHistogram->Get()->GetSRV());

					context.Dispatch(1);
				});

		graph.ExportBuffer(pAverageLuminance, &m_pAverageLuminance);
	}

	RGTexture* pBloomTexture = graph.ImportTexture(m_pBloomTexture);

	if (Tweakables::g_Bloom.Get())
	{
		RG_GRAPH_SCOPE("Bloom", graph);

		graph.AddPass("Separate Bloom", RGPassFlag::Compute)
			.Read({ sceneTextures.pColorTarget, pAverageLuminance })
			.Write(pBloomTexture)
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					Texture* pTarget = pBloomTexture->Get();
					RefCountPtr<UnorderedAccessView>* pTargetUAVs = m_pBloomUAVs.data();

					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pBloomSeparatePSO);

					struct
					{
						float Threshold;
						float BrightnessClamp;
					} parameters;

					parameters.Threshold = Tweakables::g_BloomThreshold;
					parameters.BrightnessClamp = Tweakables::g_BloomMaxBrightness;

					context.SetRootConstants(0, parameters);
					context.SetRootCBV(1, Renderer::GetViewUniforms(pView));

					context.BindResources(2, {
						pTargetUAVs[0]
						});
					context.BindResources(3, {
						sceneTextures.pColorTarget->Get()->GetSRV(),
						pAverageLuminance->Get()->GetSRV(),
						});;

					context.Dispatch(ComputeUtils::GetNumThreadGroups(pTarget->GetWidth(), 8, pTarget->GetHeight(), 8));
				});

		graph.AddPass("Bloom Mip Chain", RGPassFlag::Compute)
			.Write(pBloomTexture)
			.Bind([=](CommandContext& context, const RGPassResources& /*resources*/)
				{
					Texture* pSource = pBloomTexture->Get();
					Texture* pTarget = m_pBloomIntermediateTexture;

					RefCountPtr<UnorderedAccessView>* pSourceUAVs = m_pBloomUAVs.data();
					RefCountPtr<UnorderedAccessView>* pTargetUAVs = m_pBloomIntermediateUAVs.data();

					context.SetComputeRootSignature(m_pCommonRS);
					context.SetPipelineState(m_pBloomMipChainPSO);

					context.SetRootCBV(1, Renderer::GetViewUniforms(pView));

					uint32 width = pTarget->GetWidth() / 2;
					uint32 height = pTarget->GetHeight() / 2;

					const uint32 numMips = pTarget->GetMipLevels();
					constexpr uint32 ThreadGroupSize = 128;

					for (uint32 i = 1; i < numMips; ++i)
					{
						struct
						{
							uint32 SourceMip;
							Vector2 TargetDimensionsInv;
							uint32 Horizontal;
						} parameters;

						parameters.TargetDimensionsInv = Vector2(1.0f / width, 1.0f / height);

						for (uint32 direction = 0; direction < 2; ++direction)
						{
							context.InsertResourceBarrier(pSource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
							context.InsertResourceBarrier(pTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

							parameters.SourceMip = direction == 0 ? i - 1 : i;
							parameters.Horizontal = direction;

							context.SetRootConstants(0, parameters);
							context.BindResources(2, pTargetUAVs[i].Get());
							context.BindResources(3, pSource->GetSRV());

							IntVector3 numThreadGroups = direction == 0 ?
								ComputeUtils::GetNumThreadGroups(width, 1, height, ThreadGroupSize) :
								ComputeUtils::GetNumThreadGroups(width, ThreadGroupSize, height, 1);
							context.Dispatch(numThreadGroups);

							std::swap(pSource, pTarget);
							std::swap(pSourceUAVs, pTargetUAVs);
						}

						width /= 2;
						height /= 2;
					}
				});
	}

	RGTexture* pTonemapTarget = graph.CreateTexture("Tonemap Target", TextureDesc::CreateRenderTarget(viewDimensions.x, viewDimensions.y, DXGI_FORMAT_R8G8B8A8_UNORM));

	graph.AddPass("Tonemap", RGPassFlag::Compute)
		.Read({ sceneTextures.pColorTarget, pAverageLuminance, pBloomTexture })
		.Write(pTonemapTarget)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
			{
				Texture* pTarget = pTonemapTarget->Get();

				struct
				{
					float WhitePoint;
					uint32 Tonemapper;
				} constBuffer;
				constBuffer.WhitePoint = Tweakables::g_WhitePoint.Get();
				constBuffer.Tonemapper = Tweakables::g_ToneMapper.Get();

				context.SetPipelineState(m_pToneMapPSO);
				context.SetComputeRootSignature(m_pCommonRS);

				context.SetRootConstants(0, constBuffer);
				context.SetRootCBV(1, Renderer::GetViewUniforms(pView, pTarget));
				context.BindResources(2, pTarget->GetUAV());
				context.BindResources(3, {
					sceneTextures.pColorTarget->Get()->GetSRV(),
					pAverageLuminance->Get()->GetSRV(),
					Tweakables::g_Bloom.Get() ? pBloomTexture->Get()->GetSRV() : GraphicsCommon::GetDefaultTexture(DefaultTexture::Black2D)->GetSRV(),
					});
				context.Dispatch(ComputeUtils::GetNumThreadGroups(pTarget->GetWidth(), 16, pTarget->GetHeight(), 16));
			});

	sceneTextures.pColorTarget = pTonemapTarget;

	if (Tweakables::g_DrawHistogram.Get())
	{
		if(!m_pDebugHistogramTexture)
			m_pDebugHistogramTexture = m_pDevice->CreateTexture(TextureDesc::Create2D(256 * 4, 256, DXGI_FORMAT_R8G8B8A8_UNORM, TextureFlag::ShaderResource | TextureFlag::UnorderedAccess), "Debug Histogram");

		graph.AddPass("Draw Histogram", RGPassFlag::Compute)
			.Read({ pLuminanceHistogram, pAverageLuminance })
			.Bind([=](CommandContext& context, const RGPassResources& resources)
				{
					Texture* pTarget = m_pDebugHistogramTexture;
					context.InsertResourceBarrier(pTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

					context.ClearUavUInt(pTarget, pTarget->GetUAV());

					context.SetPipelineState(m_pDrawHistogramPSO);
					context.SetComputeRootSignature(m_pCommonRS);

					struct
					{
						float MinLogLuminance;
						float InverseLogLuminanceRange;
						Vector2 InvTextureDimensions;
					} parameters;

					parameters.MinLogLuminance = Tweakables::g_MinLogLuminance.Get();
					parameters.InverseLogLuminanceRange = 1.0f / (Tweakables::g_MaxLogLuminance.Get() - Tweakables::g_MinLogLuminance.Get());
					parameters.InvTextureDimensions.x = 1.0f / pTarget->GetWidth();
					parameters.InvTextureDimensions.y = 1.0f / pTarget->GetHeight();

					context.SetRootConstants(0, parameters);
					context.BindResources(2, pTarget->GetUAV());
					context.BindResources(3, {
						pLuminanceHistogram->Get()->GetSRV(),
						pAverageLuminance->Get()->GetSRV(),
						});

					context.Dispatch(1, pLuminanceHistogram->Get()->GetNumElements());
					context.InsertResourceBarrier(pTarget, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
				});
	}

	if (Tweakables::g_VisualizeLightDensity)
	{
		if (m_RenderPath == RenderPath::Clustered)
		{
			m_pClusteredForward->VisualizeLightDensity(graph, pView, sceneTextures);
		}
		else if (m_RenderPath == RenderPath::Tiled)
		{
			m_pTiledForward->VisualizeLightDensity(graph, m_pDevice, pView, sceneTextures);
		}
	}

	if (Tweakables::g_VisualizeDDGI)
	{
		for (uint32 i = 0; i < m_World.DDGIVolumes.size(); ++i)
		{
			const DDGIVolume& ddgi = m_World.DDGIVolumes[i];
			graph.AddPass("DDGI Visualize", RGPassFlag::Raster)
				.DepthStencil(sceneTextures.pDepth, RenderTargetLoadAction::Load, true)
				.RenderTarget(sceneTextures.pColorTarget, RenderTargetLoadAction::Load)
				.Bind([=](CommandContext& context, const RGPassResources& resources)
					{
						context.BeginRenderPass(resources.GetRenderPassInfo());

						context.SetGraphicsRootSignature(m_pCommonRS);
						context.SetPipelineState(m_pDDGIVisualizePSO);
						context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

						struct
						{
							uint32 VolumeIndex;
						} parameters;
						parameters.VolumeIndex = i;

						context.SetRootConstants(0, parameters);
						context.SetRootCBV(1, Renderer::GetViewUniforms(pView));
						context.Draw(0, 2880, ddgi.NumProbes.x * ddgi.NumProbes.y * ddgi.NumProbes.z);

						context.EndRenderPass();
					});
		}
	}

	RGTexture* pFinalOutput = graph.ImportTexture(m_ColorOutput);

	graph.AddPass("Copy Final", RGPassFlag::Copy)
		.Read(sceneTextures.pColorTarget)
		.Write(pFinalOutput)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
			{
				context.CopyResource(sceneTextures.pColorTarget->Get(), pFinalOutput->Get());
				context.InsertResourceBarrier(pFinalOutput->Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			});

	Texture* pBackbuffer = m_pSwapchain->GetBackBuffer();
	ImGuiRenderer::Render(graph, pBackbuffer);

	graph.AddPass("Transition", RGPassFlag::NeverCull)
		.Read(sceneTextures.pColorTarget)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
			{
				context.InsertResourceBarrier(m_pSwapchain->GetBackBuffer(), D3D12_RESOURCE_STATE_PRESENT);
			});

	graph.Compile();
	if (Tweakables::g_DumpRenderGraph)
	{
		graph.DumpGraph(Sprintf("%sRenderGraph.html", Paths::SavedDir().c_str()).c_str());
		Tweakables::g_DumpRenderGraph = false;
	}
	graph.Execute();

	PROFILE_END();

	Profiler::Get()->Resolve(m_pDevice);
	m_pDevice->TickFrame();
	m_pSwapchain->Present(true);
	++m_Frame;

	if (m_CapturePix)
	{
		D3D::EnqueuePIXCapture();
		m_CapturePix = false;
	}
}

void DemoApp::OnResizeOrMove(int width, int height)
{
	E_LOG(Info, "Window resized: %dx%d", width, height);
	m_pSwapchain->OnResizeOrMove(width, height);
}

void DemoApp::OnResizeViewport(int width, int height)
{
	E_LOG(Info, "Viewport resized: %dx%d", width, height);

	m_ColorOutput = m_pDevice->CreateTexture(TextureDesc::Create2D(width, height, DXGI_FORMAT_R8G8B8A8_UNORM), "Final Target");

	for (uint32 i = 0; i < SwapChain::NUM_FRAMES; ++i)
	{
		RefCountPtr<Buffer> pBuffer = m_pDevice->CreateBuffer(BufferDesc::CreateTyped(1, DXGI_FORMAT_R32G32_FLOAT, BufferFlag::Readback), "SDSM Reduction Readback Target");
		m_ReductionReadbackTargets.push_back(std::move(pBuffer));
	}

	uint32 mips = Math::Min(5u, (uint32)log2f((float)Math::Max(width, height)));
	TextureDesc bloomDesc = TextureDesc::Create2D(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, TextureFlag::ShaderResource | TextureFlag::UnorderedAccess, 1, mips);
	m_pBloomTexture = m_pDevice->CreateTexture(bloomDesc, "Bloom");
	m_pBloomIntermediateTexture = m_pDevice->CreateTexture(bloomDesc, "Bloom Intermediate");

	m_pBloomUAVs.resize(mips);
	m_pBloomIntermediateUAVs.resize(mips);
	for (uint32 i = 0; i < mips; ++i)
	{
		m_pBloomUAVs[i] = m_pDevice->CreateUAV(m_pBloomTexture, TextureUAVDesc((uint8)i));
		m_pBloomIntermediateUAVs[i] = m_pDevice->CreateUAV(m_pBloomIntermediateTexture, TextureUAVDesc((uint8)i));
	}

	m_pCamera->SetViewport(FloatRect(0, 0, (float)width, (float)height));
}

void DemoApp::InitializePipelines()
{
	// Common Root Signature - Make it 12 DWORDs as is often recommended by IHVs
	m_pCommonRS = new RootSignature(m_pDevice);
	m_pCommonRS->AddRootConstants(0, 8);
	m_pCommonRS->AddConstantBufferView(100);
	m_pCommonRS->AddDescriptorTableSimple(0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6);
	m_pCommonRS->AddDescriptorTableSimple(0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6);
	m_pCommonRS->Finalize("Common");

	//Shadow mapping - Vertex shader-only pass that writes to the depth buffer using the light matrix
	{
		PipelineStateInitializer psoDesc;
		psoDesc.SetRootSignature(m_pCommonRS);
		psoDesc.SetVertexShader("DepthOnly.hlsl", "VSMain");
		psoDesc.SetRenderTargetFormats({}, DEPTH_STENCIL_SHADOW_FORMAT, 1);
		psoDesc.SetCullMode(D3D12_CULL_MODE_NONE);
		psoDesc.SetDepthTest(D3D12_COMPARISON_FUNC_GREATER);
		psoDesc.SetDepthBias(-10, 0, -4.0f);
		psoDesc.SetName("Shadow Mapping Opaque");
		m_pShadowsOpaquePSO = m_pDevice->CreatePipeline(psoDesc);

		psoDesc.SetPixelShader("DepthOnly.hlsl", "PSMain");
		psoDesc.SetName("Shadow Mapping Alpha Mask");
		m_pShadowsAlphaMaskPSO = m_pDevice->CreatePipeline(psoDesc);
	}

	//Depth prepass - Simple vertex shader to fill the depth buffer to optimize later passes
	{
		PipelineStateInitializer psoDesc;
		psoDesc.SetRootSignature(m_pCommonRS);
		psoDesc.SetVertexShader("DepthOnly.hlsl", "VSMain");
		psoDesc.SetDepthTest(D3D12_COMPARISON_FUNC_GREATER);
		psoDesc.SetRenderTargetFormats({}, DXGI_FORMAT_D32_FLOAT, 1);
		psoDesc.SetName("Depth Prepass Opaque");
		m_pDepthPrepassOpaquePSO = m_pDevice->CreatePipeline(psoDesc);

		psoDesc.SetPixelShader("DepthOnly.hlsl", "PSMain");
		psoDesc.SetName("Depth Prepass Alpha Mask");
		psoDesc.SetCullMode(D3D12_CULL_MODE_NONE);
		m_pDepthPrepassAlphaMaskPSO = m_pDevice->CreatePipeline(psoDesc);
	}

	m_pLuminanceHistogramPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "LuminanceHistogram.hlsl", "CSMain");
	m_pDrawHistogramPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "DrawLuminanceHistogram.hlsl", "DrawLuminanceHistogram");
	m_pAverageLuminancePSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "AverageLuminance.hlsl", "CSMain");

	//Depth resolve
	m_pResolveDepthPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "ResolveDepth.hlsl", "CSMain", { "DEPTH_RESOLVE_MIN" });
	m_pPrepareReduceDepthPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "ReduceDepth.hlsl", "PrepareReduceDepth");
	m_pPrepareReduceDepthMsaaPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "ReduceDepth.hlsl", "PrepareReduceDepth", { "WITH_MSAA" });
	m_pReduceDepthPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "ReduceDepth.hlsl", "ReduceDepth");

	m_pToneMapPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "Tonemapping.hlsl", "CSMain");
	m_pCameraMotionPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "CameraMotionVectors.hlsl", "CSMain");
	m_pTemporalResolvePSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "TemporalResolve.hlsl", "CSMain");

	m_pGenerateMipsPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "GenerateMips.hlsl", "CSMain");

	//Sky
	{
		PipelineStateInitializer psoDesc;
		psoDesc.SetRootSignature(m_pCommonRS);
		psoDesc.SetVertexShader("ProceduralSky.hlsl", "VSMain");
		psoDesc.SetPixelShader("ProceduralSky.hlsl", "PSMain");
		psoDesc.SetRenderTargetFormats(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT, 1);
		psoDesc.SetDepthTest(D3D12_COMPARISON_FUNC_GREATER);
		psoDesc.SetDepthWrite(false);
		psoDesc.SetName("Skybox");
		m_pSkyboxPSO = m_pDevice->CreatePipeline(psoDesc);

		m_pRenderSkyPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "ProceduralSky.hlsl", "ComputeSkyCS");
	}

	//Bloom
	m_pBloomSeparatePSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "Bloom.hlsl", "SeparateBloomCS");
	m_pBloomMipChainPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "Bloom.hlsl", "BloomMipChainCS");

	//Visibility Rendering
	if (m_pDevice->GetCapabilities().SupportsMeshShading())
	{
		//Pipeline state
		PipelineStateInitializer psoDesc;
		psoDesc.SetRootSignature(m_pCommonRS);
		psoDesc.SetAmplificationShader("VisibilityRendering.hlsl", "ASMain");
		psoDesc.SetMeshShader("VisibilityRendering.hlsl", "MSMain");
		psoDesc.SetPixelShader("VisibilityRendering.hlsl", "PSMain");
		psoDesc.SetDepthTest(D3D12_COMPARISON_FUNC_GREATER);
		psoDesc.SetRenderTargetFormats(DXGI_FORMAT_R32_UINT, DXGI_FORMAT_D32_FLOAT, 1);
		psoDesc.SetName("Visibility Rendering");
		m_pVisibilityRenderingPSO = m_pDevice->CreatePipeline(psoDesc);

		psoDesc.SetCullMode(D3D12_CULL_MODE_NONE);
		psoDesc.SetPixelShader("VisibilityRendering.hlsl", "PSMain", { "ALPHA_TEST" });
		psoDesc.SetName("Visibility Rendering Masked");
		m_pVisibilityRenderingMaskedPSO = m_pDevice->CreatePipeline(psoDesc);

		//Visibility Shading
		m_pVisibilityShadingPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "VisibilityShading.hlsl", "CSMain");
	}

	// DDGI
	if (m_pDevice->GetCapabilities().SupportsRaytracing())
	{
		m_pDDGIUpdateIrradianceColorPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "RayTracing/DDGI.hlsl", "UpdateIrradianceCS");
		m_pDDGIUpdateIrradianceDepthPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "RayTracing/DDGI.hlsl", "UpdateDepthCS");
		m_pDDGIUpdateProbeStatesPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "RayTracing/DDGI.hlsl", "UpdateProbeStatesCS");

		StateObjectInitializer soDesc{};
		soDesc.Name = "DDGI Trace Rays";
		soDesc.MaxRecursion = 1;
		soDesc.MaxPayloadSize = 6 * sizeof(float);
		soDesc.MaxAttributeSize = 2 * sizeof(float);
		soDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
		soDesc.AddLibrary("RayTracing/DDGIRayTrace.hlsl", { "TraceRaysRGS" });
		soDesc.AddLibrary("RayTracing/SharedRaytracingLib.hlsl", { "OcclusionMS", "MaterialCHS", "MaterialAHS", "MaterialMS" });
		soDesc.AddHitGroup("MaterialHG", "MaterialCHS", "MaterialAHS");
		soDesc.AddMissShader("MaterialMS");
		soDesc.AddMissShader("OcclusionMiss");
		soDesc.pGlobalRootSignature = m_pCommonRS;
		m_pDDGITraceRaysSO = m_pDevice->CreateStateObject(soDesc);

		PipelineStateInitializer psoDesc;
		psoDesc.SetRootSignature(m_pCommonRS);
		psoDesc.SetVertexShader("RayTracing/DDGI.hlsl", "VisualizeIrradianceVS");
		psoDesc.SetPixelShader("RayTracing/DDGI.hlsl", "VisualizeIrradiancePS");
		psoDesc.SetDepthTest(D3D12_COMPARISON_FUNC_GREATER);
		psoDesc.SetRenderTargetFormats(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT, 1);
		psoDesc.SetName("Visualize Irradiance");
		psoDesc.SetCullMode(D3D12_CULL_MODE_NONE);
		m_pDDGIVisualizePSO = m_pDevice->CreatePipeline(psoDesc);
	}

	// Texture visualize
	m_pVisualizeTexturePSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "ImageVisualize.hlsl", "CSMain");
}

void DemoApp::VisualizeTexture(RGGraph& graph, RGTexture* pTexture)
{
	const TextureDesc& desc = pTexture->GetDesc();
	RGTexture* pTarget = graph.CreateTexture("Visualize Target", TextureDesc::Create2D(desc.Width, desc.Height, DXGI_FORMAT_R8G8B8A8_UNORM));

	if (ImGui::Begin("Visualize Texture") && m_VisualizeTextureData.pVisualizeTexture)
	{
		ImGui::Text("%s - Resolution: %dx%d", pTexture->GetName(), desc.Width, desc.Height);
		ImGui::DragFloatRange2("Range", &m_VisualizeTextureData.RangeMin, &m_VisualizeTextureData.RangeMax, 0.02f, 0, 10);
		if (desc.Mips > 1)
		{
			ImGui::SliderFloat("Mip", &m_VisualizeTextureData.MipLevel, 0, (float)desc.Mips - 1);
		}
		if (desc.DepthOrArraySize > 1)
		{
			ImGui::SliderFloat("Slice", &m_VisualizeTextureData.Slice, 0, (float)desc.DepthOrArraySize - 1);
		}
		ImGui::Checkbox("R", &m_VisualizeTextureData.VisibleChannels[0]);
		ImGui::SameLine();
		ImGui::Checkbox("G", &m_VisualizeTextureData.VisibleChannels[1]);
		ImGui::SameLine();
		ImGui::Checkbox("B", &m_VisualizeTextureData.VisibleChannels[2]);
		ImGui::SameLine();
		ImGui::Checkbox("A", &m_VisualizeTextureData.VisibleChannels[3]);

		ImGui::ImageAutoSize(m_VisualizeTextureData.pVisualizeTexture, ImVec2((float)desc.Width, (float)desc.Height));
	}
	ImGui::End();

	graph.AddPass("Process Image Visualizer", RGPassFlag::Compute | RGPassFlag::NeverCull)
		.Read(pTexture)
		.Write(pTarget)
		.Bind([=](CommandContext& context, const RGPassResources& /*resources*/)
			{
				context.SetComputeRootSignature(m_pCommonRS);
				context.SetPipelineState(m_pVisualizeTexturePSO);

				struct ConstantsData
				{
					Vector2 InvDimensions;
					Vector2 ValueRange;
					uint32 TextureSource;
					uint32 TextureTarget;
					TextureDimension TextureType;
					uint32 ChannelMask;
					float MipLevel;
					float Slice;
				} constants;

				const TextureDesc& desc = pTexture->GetDesc();
				constants.TextureSource = pTexture->Get()->GetSRV()->GetHeapIndex();
				constants.TextureTarget = pTarget->Get()->GetUAV()->GetHeapIndex();
				constants.InvDimensions.x = 1.0f / desc.Width;
				constants.InvDimensions.y = 1.0f / desc.Height;
				constants.TextureType = pTexture->GetDesc().Dimensions;
				constants.ValueRange = Vector2(m_VisualizeTextureData.RangeMin, m_VisualizeTextureData.RangeMax);
				constants.ChannelMask =
					(m_VisualizeTextureData.VisibleChannels[0] ? 1 : 0) << 0 |
					(m_VisualizeTextureData.VisibleChannels[1] ? 1 : 0) << 1 |
					(m_VisualizeTextureData.VisibleChannels[2] ? 1 : 0) << 2 |
					(m_VisualizeTextureData.VisibleChannels[3] ? 1 : 0) << 3;
				constants.MipLevel = m_VisualizeTextureData.MipLevel;
				constants.Slice = m_VisualizeTextureData.Slice / desc.DepthOrArraySize;

				context.SetRootCBV(1, constants);

				context.Dispatch(ComputeUtils::GetNumThreadGroups(desc.Width, 16, desc.Height, 16));
			});

	graph.ExportTexture(pTarget, &m_VisualizeTextureData.pVisualizeTexture);
}

void DemoApp::UpdateImGui()
{
	m_FrameTimes[m_Frame % m_FrameTimes.size()] = Time::DeltaTime();

	static ImGuiConsole console;
	static bool showProfiler = false;
	static bool showImguiDemo = false;

	ImGuiViewport* pViewport = ImGui::GetMainViewport();
	ImGuiID dockspace = ImGui::DockSpaceOverViewport(pViewport);

	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu(ICON_FA_FILE " File"))
		{
			if (ImGui::MenuItem(ICON_FA_FILE " Load Mesh", nullptr, nullptr))
			{
				OPENFILENAME ofn{};
				TCHAR szFile[260]{};
				ofn.lStructSize = sizeof(ofn);
				ofn.hwndOwner = m_Window;
				ofn.lpstrFile = szFile;
				ofn.nMaxFile = sizeof(szFile);
				ofn.lpstrFilter = "Supported files (*.gltf;*.dat;*.ldr;*.mpd)\0*.gltf;*.dat;*.ldr;*.mpd\0All Files (*.*)\0*.*\0";;
				ofn.nFilterIndex = 1;
				ofn.lpstrFileTitle = NULL;
				ofn.nMaxFileTitle = 0;
				ofn.lpstrInitialDir = NULL;
				ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

				if (GetOpenFileNameA(&ofn) == TRUE)
				{
					m_World.Meshes.clear();
					CommandContext* pContext = m_pDevice->AllocateCommandContext();
					LoadMesh(ofn.lpstrFile, *pContext, m_World);
					pContext->Execute(true);
				}
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(ICON_FA_WINDOW_MAXIMIZE " Windows"))
		{
			if (ImGui::MenuItem(ICON_FA_CLOCK_O " Profiler", 0, showProfiler))
			{
				showProfiler = !showProfiler;
			}
			bool& showConsole = console.IsVisible();
			if (ImGui::MenuItem("Output Log", 0, showConsole))
			{
				showConsole = !showConsole;
			}
			if (ImGui::MenuItem("Luminance Histogram", 0, &Tweakables::g_DrawHistogram.Get()))
			{
				Tweakables::g_VisualizeShadowCascades.Set(!Tweakables::g_DrawHistogram.GetBool());
			}

			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(ICON_FA_WRENCH " Tools"))
		{
			if (ImGui::MenuItem("Dump RenderGraph"))
			{
				Tweakables::g_DumpRenderGraph = true;
			}
			if (ImGui::MenuItem("Screenshot"))
			{
				Tweakables::g_Screenshot = true;
			}
			if (ImGui::MenuItem("Pix Capture"))
			{
				m_CapturePix = true;
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(ICON_FA_QUESTION " Help"))
		{
			if (ImGui::MenuItem("ImGui Demo", 0, showImguiDemo))
			{
				showImguiDemo = !showImguiDemo;
			}
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	ImGui::SetNextWindowDockID(dockspace, ImGuiCond_FirstUseEver);
	ImGui::Begin("Viewport", 0, ImGuiWindowFlags_NoScrollbar);
	ImVec2 viewportPos = ImGui::GetWindowPos();
	ImVec2 viewportSize = ImGui::GetWindowSize();
	float widthDelta = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
	float heightDelta = ImGui::GetWindowContentRegionMax().y - ImGui::GetWindowContentRegionMin().y;
	uint32 width = (uint32)Math::Max(16.0f, widthDelta);
	uint32 height = (uint32)Math::Max(16.0f, heightDelta);

	if (width != m_ColorOutput->GetWidth() || height != m_ColorOutput->GetHeight())
	{
		OnResizeViewport(width, height);
	}
	ImGuizmo::SetRect(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, (float)width, (float)height);
		ImGui::Image(m_ColorOutput, ImVec2((float)width, (float)height));
	ImGui::End();

	if (Tweakables::g_VisualizeLightDensity)
	{
		//Render Color Legend
		ImGui::SetNextWindowSize(ImVec2(60, 255));

		ImGui::SetNextWindowPos(ImVec2(viewportPos.x + viewportSize.x - 65, viewportPos.y + viewportSize.y - 280));
		ImGui::Begin("Visualize Light Density", 0, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
		ImGui::SetWindowFontScale(1.2f);
		ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
		static uint32 DEBUG_COLORS[] = {
			IM_COL32(0,4,141, 255),
			IM_COL32(5,10,255, 255),
			IM_COL32(0,164,255, 255),
			IM_COL32(0,255,189, 255),
			IM_COL32(0,255,41, 255),
			IM_COL32(117,254,1, 255),
			IM_COL32(255,239,0, 255),
			IM_COL32(255,86,0, 255),
			IM_COL32(204,3,0, 255),
			IM_COL32(65,0,1, 255),
		};

		for (uint32 i = 0; i < ARRAYSIZE(DEBUG_COLORS); ++i)
		{
			char number[16];
			FormatString(number, ARRAYSIZE(number), "%d", i);
			ImGui::PushStyleColor(ImGuiCol_Button, DEBUG_COLORS[i]);
			ImGui::Button(number, ImVec2(40, 20));
			ImGui::PopStyleColor();
		}
		ImGui::PopStyleColor();
		ImGui::End();
	}

	console.Update(ImVec2(300, (float)pViewport->Size.x), ImVec2((float)pViewport->Size.x - 300 * 2, 250));

	if (showImguiDemo)
	{
		ImGui::ShowDemoWindow();
	}

	if (Tweakables::g_DrawHistogram && m_pDebugHistogramTexture)
	{
		ImGui::Begin("Luminance Histogram");
		ImVec2 cursor = ImGui::GetCursorPos();
		ImGui::ImageAutoSize(m_pDebugHistogramTexture, ImVec2((float)m_pDebugHistogramTexture->GetWidth(), (float)m_pDebugHistogramTexture->GetHeight()));
		ImGui::GetWindowDrawList()->AddText(cursor, IM_COL32(255, 255, 255, 255), Sprintf("%.2f", Tweakables::g_MinLogLuminance.Get()).c_str());
		ImGui::End();
	}

	if (Tweakables::g_VisualizeShadowCascades)
	{
		float imageSize = 230;
		if (ImGui::Begin("Shadow Cascades"))
		{
			const Light& sunLight = m_World.Lights[0];
			for (int i = 0; i < Tweakables::g_ShadowCascades; ++i)
			{
				ImGui::Image(sunLight.ShadowMaps[i], ImVec2(imageSize, imageSize));
				ImGui::SameLine();
			}
		}
		ImGui::End();
	}

	if (showProfiler)
	{
		if (ImGui::Begin("Profiler", &showProfiler))
		{
			ImGui::Text("MS: %4.2f | FPS: %4.2f | %d x %d", Time::DeltaTime() * 1000.0f, 1.0f / Time::DeltaTime(), m_SceneData.GetDimensions().x, m_SceneData.GetDimensions().y);
			ImGui::PlotLines("", m_FrameTimes.data(), (int)m_FrameTimes.size(), m_Frame % m_FrameTimes.size(), 0, 0.0f, 0.03f, ImVec2(ImGui::GetContentRegionAvail().x, 100));

			if (ImGui::TreeNodeEx("Profiler", ImGuiTreeNodeFlags_DefaultOpen))
			{
				Profiler::Get()->DrawImGui();
				ImGui::TreePop();
			}
		}
		ImGui::End();
	}

	if (ImGui::Begin("Parameters"))
	{
		if (ImGui::CollapsingHeader("Global"))
		{
			ImGui::Combo("Render Path", (int*)&m_RenderPath, [](void* /*data*/, int index, const char** outText)
				{
					RenderPath p = (RenderPath)index;
					switch (p)
					{
					case RenderPath::Tiled:
						*outText = "Tiled";
						break;
					case RenderPath::Clustered:
						*outText = "Clustered";
						break;
					case RenderPath::PathTracing:
						*outText = "Path Tracing";
						break;
					case RenderPath::Visibility:
						*outText = "Visibility";
						break;
					default:
						noEntry();
						break;
					}
					return true;
				}, nullptr, (int)RenderPath::MAX);

			ImGui::Text("Camera");
			ImGui::Text("Location: [%.2f, %.2f, %.2f]", m_pCamera->GetPosition().x, m_pCamera->GetPosition().y, m_pCamera->GetPosition().z);
			float fov = m_pCamera->GetFoV();
			if (ImGui::SliderAngle("Field of View", &fov, 10, 120))
			{
				m_pCamera->SetFoV(fov);
			}
			Vector2 farNear(m_pCamera->GetFar(), m_pCamera->GetNear());
			if (ImGui::DragFloatRange2("Near/Far", &farNear.x, &farNear.y, 1, 0.1f, 100))
			{
				m_pCamera->SetFarPlane(farNear.x);
				m_pCamera->SetNearPlane(farNear.y);
			}
		}

		if (ImGui::CollapsingHeader("Sky"))
		{
			ImGui::SliderFloat("Sun Orientation", &Tweakables::g_SunOrientation, -Math::PI, Math::PI);
			ImGui::SliderFloat("Sun Inclination", &Tweakables::g_SunInclination, 0, 1);
			ImGui::SliderFloat("Sun Temperature", &Tweakables::g_SunTemperature, 1000, 15000);
			ImGui::SliderFloat("Sun Intensity", &Tweakables::g_SunIntensity, 0, 30);
			ImGui::Checkbox("Volumetric Fog", &Tweakables::g_VolumetricFog.Get());
		}

		if (ImGui::CollapsingHeader("Shadows"))
		{
			ImGui::SliderInt("Shadow Cascades", &Tweakables::g_ShadowCascades.Get(), 1, 4);
			ImGui::Checkbox("SDSM", &Tweakables::g_SDSM.Get());
			ImGui::SliderFloat("PSSM Factor", &Tweakables::g_PSSMFactor.Get(), 0, 1);
			ImGui::Checkbox("Visualize Cascades", &Tweakables::g_VisualizeShadowCascades.Get());
		}
		if (ImGui::CollapsingHeader("Bloom"))
		{
			ImGui::Checkbox("Enabled", &Tweakables::g_Bloom.Get());
			ImGui::SliderFloat("Brightness Threshold", &Tweakables::g_BloomThreshold.Get(), 0, 5);
			ImGui::SliderFloat("Max Brightness", &Tweakables::g_BloomMaxBrightness.Get(), 1, 100);
		}
		if (ImGui::CollapsingHeader("Exposure/Tonemapping"))
		{
			ImGui::DragFloatRange2("Log Luminance", &Tweakables::g_MinLogLuminance.Get(), &Tweakables::g_MaxLogLuminance.Get(), 1.0f, -100, 50);
			ImGui::Checkbox("Draw Exposure Histogram", &Tweakables::g_DrawHistogram.Get());
			ImGui::SliderFloat("White Point", &Tweakables::g_WhitePoint.Get(), 0, 20);
			ImGui::SliderFloat("Tau", &Tweakables::g_Tau.Get(), 0, 5);

			ImGui::Combo("Tonemapper", (int*)&Tweakables::g_ToneMapper.Get(), [](void* /*data*/, int index, const char** outText)
				{
					constexpr static const char* tonemappers[] = {
						"Reinhard",
						"Reinhard Extended",
						"ACES Fast",
						"Unreal 3",
						"Uncharted 2",
					};

					if (index < (int)ARRAYSIZE(tonemappers))
					{
						*outText = tonemappers[index];
						return true;
					}
					noEntry();
					return false;
				}, nullptr, 5);
		}

		if (ImGui::CollapsingHeader("Misc"))
		{
			ImGui::Checkbox("TAA", &Tweakables::g_TAA.Get());
			ImGui::Checkbox("Debug Render Lights", &Tweakables::g_VisualizeLights.Get());
			ImGui::Checkbox("Visualize Light Density", &Tweakables::g_VisualizeLightDensity.Get());
			extern bool g_VisualizeClusters;
			ImGui::Checkbox("Visualize Clusters", &g_VisualizeClusters);
			ImGui::SliderInt("SSR Samples", &Tweakables::g_SsrSamples.Get(), 0, 32);
			ImGui::Checkbox("Object Bounds", &Tweakables::g_RenderObjectBounds.Get());
			ImGui::Checkbox("Render Terrain", &Tweakables::g_RenderTerrain.Get());
			ImGui::Checkbox("Freeze Cluster Culling", &Tweakables::g_FreezeClusterCulling.Get());
		}

		if (ImGui::CollapsingHeader("Raytracing"))
		{
			if (m_pDevice->GetCapabilities().SupportsRaytracing())
			{
				ImGui::Checkbox("Raytraced AO", &Tweakables::g_RaytracedAO.Get());
				ImGui::Checkbox("Raytraced Reflections", &Tweakables::g_RaytracedReflections.Get());
				ImGui::Checkbox("DDGI", &Tweakables::g_EnableDDGI.Get());
				if (m_World.DDGIVolumes.size() > 0)
					ImGui::SliderInt("DDGI RayCount", &m_World.DDGIVolumes.front().NumRays, 1, m_World.DDGIVolumes.front().MaxNumRays);
				ImGui::Checkbox("Visualize DDGI", &Tweakables::g_VisualizeDDGI.Get());
				ImGui::SliderAngle("TLAS Bounds Threshold", &Tweakables::g_TLASBoundsThreshold.Get(), 0, 40);
			}
		}
	}
	ImGui::End();
}

void DemoApp::LoadMesh(const std::string& filePath, CommandContext& context, World& world)
{
	std::unique_ptr<Mesh> pMesh = std::make_unique<Mesh>();
	pMesh->Load(filePath.c_str(), m_pDevice, &context, 1.0f);
	world.Meshes.push_back(std::move(pMesh));
}

void DemoApp::CreateShadowViews(SceneView& view, World& world)
{
	PROFILE_SCOPE("Shadow Setup");

	float minPoint = 0;
	float maxPoint = 1;

	const uint32 numCascades = Tweakables::g_ShadowCascades;
	const float pssmLambda = Tweakables::g_PSSMFactor;
	view.NumShadowCascades = numCascades;

	if (Tweakables::g_SDSM)
	{
		Buffer* pSourceBuffer = m_ReductionReadbackTargets[(m_Frame + 1) % SwapChain::NUM_FRAMES];
		Vector2* pData = (Vector2*)pSourceBuffer->GetMappedData();
		minPoint = pData->x;
		maxPoint = pData->y;
	}

	float n = m_pCamera->GetNear();
	float f = m_pCamera->GetFar();
	float nearPlane = Math::Min(n, f);
	float farPlane = Math::Max(n, f);
	float clipPlaneRange = farPlane - nearPlane;

	float minZ = nearPlane + minPoint * clipPlaneRange;
	float maxZ = nearPlane + maxPoint * clipPlaneRange;

	constexpr uint32 MAX_CASCADES = 4;
	std::array<float, MAX_CASCADES> cascadeSplits{};

	for (uint32 i = 0; i < numCascades; ++i)
	{
		float p = (i + 1) / (float)numCascades;
		float log = minZ * std::pow(maxZ / minZ, p);
		float uniform = minZ + (maxZ - minZ) * p;
		float d = pssmLambda * (log - uniform) + uniform;
		cascadeSplits[i] = (d - nearPlane) / clipPlaneRange;
	}

	int32 shadowIndex = 0;
	view.ShadowViews.clear();
	auto AddShadowView = [&](Light& light, ShadowView shadowView, uint32 resolution, uint32 shadowMapLightIndex)
	{
		if (shadowMapLightIndex == 0)
		{
			light.MatrixIndex = shadowIndex;
		}
		if (shadowIndex >= (int32)m_ShadowMaps.size())
		{
			m_ShadowMaps.push_back(m_pDevice->CreateTexture(TextureDesc::CreateDepth(resolution, resolution, DEPTH_STENCIL_SHADOW_FORMAT, TextureFlag::DepthStencil | TextureFlag::ShaderResource, 1, ClearBinding(0.0f, 0)), "Shadow Map"));
		}
		RefCountPtr<Texture> pTarget = m_ShadowMaps[shadowIndex];

		light.ShadowMaps.resize(Math::Max(shadowMapLightIndex + 1, (uint32)light.ShadowMaps.size()));
		light.ShadowMaps[shadowMapLightIndex] = pTarget;
		light.ShadowMapSize = resolution;
		shadowView.pDepthTexture = pTarget;

		for (const Batch& b : view.Batches)
		{
			if (shadowView.IsPerspective)
			{
				shadowView.Visibility.AssignBit(b.InstanceData.World, shadowView.PerspectiveFrustum.Contains(b.Bounds));
			}
			else
			{
				shadowView.Visibility.AssignBit(b.InstanceData.World, shadowView.OrtographicFrustum.Contains(b.Bounds));
			}
		}

		view.ShadowViews.push_back(shadowView);
		shadowIndex++;
	};

	const Matrix vpInverse = m_pCamera->GetProjectionInverse() * m_pCamera->GetViewInverse();
	for (size_t lightIndex = 0; lightIndex < world.Lights.size(); ++lightIndex)
	{
		Light& light = world.Lights[lightIndex];
		if (!light.CastShadows)
		{
			continue;
		}
		if (light.Type == LightType::Directional)
		{
			// Frustum corners in world space
			const Vector3 frustumCornersWS[] = {
				Vector3::Transform(Vector3(-1, -1, 1), vpInverse),
				Vector3::Transform(Vector3(-1, -1, 0), vpInverse),
				Vector3::Transform(Vector3(-1, 1, 1), vpInverse),
				Vector3::Transform(Vector3(-1, 1, 0), vpInverse),
				Vector3::Transform(Vector3(1, 1, 1), vpInverse),
				Vector3::Transform(Vector3(1, 1, 0), vpInverse),
				Vector3::Transform(Vector3(1, -1, 1), vpInverse),
				Vector3::Transform(Vector3(1, -1, 0), vpInverse),
			};
			const Matrix lightView = Math::CreateLookToMatrix(Vector3::Zero, light.Direction, Vector3::Up);

			for (int i = 0; i < Tweakables::g_ShadowCascades; ++i)
			{
				float previousCascadeSplit = i == 0 ? minPoint : cascadeSplits[i - 1];
				float currentCascadeSplit = cascadeSplits[i];

				// Compute the frustum corners for the cascade in view space
				const Vector3 cornersVS[] = {
					Vector3::Transform(Vector3::Lerp(frustumCornersWS[0], frustumCornersWS[1], previousCascadeSplit), lightView),
					Vector3::Transform(Vector3::Lerp(frustumCornersWS[0], frustumCornersWS[1], currentCascadeSplit), lightView),
					Vector3::Transform(Vector3::Lerp(frustumCornersWS[2], frustumCornersWS[3], previousCascadeSplit), lightView),
					Vector3::Transform(Vector3::Lerp(frustumCornersWS[2], frustumCornersWS[3], currentCascadeSplit), lightView),
					Vector3::Transform(Vector3::Lerp(frustumCornersWS[4], frustumCornersWS[5], previousCascadeSplit), lightView),
					Vector3::Transform(Vector3::Lerp(frustumCornersWS[4], frustumCornersWS[5], currentCascadeSplit), lightView),
					Vector3::Transform(Vector3::Lerp(frustumCornersWS[6], frustumCornersWS[7], previousCascadeSplit), lightView),
					Vector3::Transform(Vector3::Lerp(frustumCornersWS[6], frustumCornersWS[7], currentCascadeSplit), lightView),
				};

				Vector3 center = Vector3::Zero;
				for (const Vector3& corner : cornersVS)
				{
					center += corner;
				}
				center /= ARRAYSIZE(cornersVS);

				//Create a bounding sphere to maintain aspect in projection to avoid flickering when rotating
				float radius = 0;
				for (const Vector3& corner : cornersVS)
				{
					float dist = Vector3::Distance(center, corner);
					radius = Math::Max(dist, radius);
				}
				Vector3 minExtents = center - Vector3(radius);
				Vector3 maxExtents = center + Vector3(radius);

				// Snap the cascade to the resolution of the shadowmap
				Vector3 extents = maxExtents - minExtents;
				Vector3 texelSize = extents / 2048;
				minExtents = Math::Floor(minExtents / texelSize) * texelSize;
				maxExtents = Math::Floor(maxExtents / texelSize) * texelSize;
				center = (minExtents + maxExtents) * 0.5f;

				// Extent the Z bounds
				float extentsZ = fabs(center.z - minExtents.z);
				extentsZ = Math::Max(extentsZ, Math::Min(1500.0f, farPlane) * 0.5f);
				minExtents.z = center.z - extentsZ;
				maxExtents.z = center.z + extentsZ;

				Matrix projectionMatrix = Math::CreateOrthographicOffCenterMatrix(minExtents.x, maxExtents.x, minExtents.y, maxExtents.y, maxExtents.z, minExtents.z);

				ShadowView shadowView;
				shadowView.IsPerspective = false;
				shadowView.ViewProjection = lightView * projectionMatrix;
				shadowView.OrtographicFrustum.Center = center;
				shadowView.OrtographicFrustum.Extents = maxExtents - minExtents;
				shadowView.OrtographicFrustum.Orientation = Quaternion::CreateFromRotationMatrix(lightView.Invert());
				static_cast<float*>(&view.ShadowCascadeDepths.x)[i] = nearPlane + currentCascadeSplit * (farPlane - nearPlane);
				AddShadowView(light, shadowView, 2048, i);
			}
		}
		else if (light.Type == LightType::Spot)
		{
			Matrix projection = Math::CreatePerspectiveMatrix(light.UmbraAngleDegrees * Math::DegreesToRadians, 1.0f, light.Range*100, 1.0f);
			Matrix lightView = Math::CreateLookToMatrix(light.Position, light.Direction, light.Direction == Vector3::Up ? Vector3::Right : Vector3::Up);

			ShadowView shadowView;
			shadowView.IsPerspective = true;
			shadowView.ViewProjection = lightView * projection;
			shadowView.PerspectiveFrustum = Math::CreateBoundingFrustum(projection, lightView);
			AddShadowView(light, shadowView, 512, 0);
		}
		else if (light.Type == LightType::Point)
		{
			Matrix viewMatrices[] = {
				Math::CreateLookToMatrix(light.Position, Vector3::Right, Vector3::Up),
				Math::CreateLookToMatrix(light.Position, Vector3::Left, Vector3::Up),
				Math::CreateLookToMatrix(light.Position, Vector3::Up, Vector3::Forward),
				Math::CreateLookToMatrix(light.Position, Vector3::Down, Vector3::Backward),
				Math::CreateLookToMatrix(light.Position, Vector3::Backward, Vector3::Up),
				Math::CreateLookToMatrix(light.Position, Vector3::Forward, Vector3::Up),
			};
			Matrix projection = Math::CreatePerspectiveMatrix(Math::PIDIV2, 1, light.Range, 1.0f);

			for (int i = 0; i < 6; ++i)
			{
				ShadowView shadowView;
				shadowView.IsPerspective = true;
				shadowView.ViewProjection = viewMatrices[i] * projection;
				shadowView.PerspectiveFrustum = Math::CreateBoundingFrustum(projection, viewMatrices[i]);
				AddShadowView(light, shadowView, 512, i);
			}
		}
	}
}