#include "stdafx.h"
#include "App.h"
#include "Core/Input.h"
#include "Core/Console.h"
#include "Core/CommandLine.h"
#include "Core/TaskQueue.h"
#include "Core/ConsoleVariables.h"
#include "Core/Window.h"
#include "Core/Profiler.h"
#include "Graphics/RHI/Graphics.h"
#include "Graphics/RHI/CommandQueue.h"
#include "Graphics/RHI/CommandContext.h"
#include "Graphics/SceneView.h"
#include "Graphics/ImGuiRenderer.h"

#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#define BREAK_ON_ALLOC 0

#ifdef LIVE_PP_PATH
#include "LPP_API_x64_CPP.h"
#define LIVE_PP() LivePPAgent livePP;
struct LivePPAgent
{
	LivePPAgent()
	{
		// create a default agent, loading the Live++ agent from the given path, e.g. "ThirdParty/LivePP"
		Agent = lpp::LppCreateDefaultAgent(LIVE_PP_PATH);
		// bail out in case the agent is not valid
		if (lpp::LppIsValidDefaultAgent(&Agent))
		{
			// enable Live++ for all loaded modules
			Agent.EnableModule(lpp::LppGetCurrentModulePath(), lpp::LPP_MODULES_OPTION_ALL_IMPORT_MODULES, nullptr, nullptr);
		}
	}

	~LivePPAgent()
	{
		if (lpp::LppIsValidDefaultAgent(&Agent))
		{
			// destroy the Live++ agent
			lpp::LppDestroyDefaultAgent(&Agent);
		}
	}

	lpp::LppDefaultAgent Agent;
};
#else
#define LIVE_PP()
#endif

int App::Run()
{
	Init_Internal();
	while (m_Window.PollMessages())
	{
		PROFILE_FRAME();

		Update_Internal();
	}
	Shutdown_Internal();
	return 0;
}

static void InitializeProfiler(GraphicsDevice* pDevice)
{
	ID3D12CommandQueue* pQueues[] =
	{
		pDevice->GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT)->GetCommandQueue(),
		pDevice->GetCommandQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE)->GetCommandQueue(),
		pDevice->GetCommandQueue(D3D12_COMMAND_LIST_TYPE_COPY)->GetCommandQueue(),
	};
	gGPUProfiler.Initialize(pDevice->GetDevice(), pQueues, ARRAYSIZE(pQueues));

	GPUProfilerCallbacks gpuCallbacks;
	gpuCallbacks.OnEventBegin = [](const char* pName, ID3D12GraphicsCommandList* pCmd, uint16 queueIndex)
	{
		gCPUProfiler.PushRegion(pName);
#if ENABLE_PIX
		::PIXBeginEvent(pCmd, 0, MULTIBYTE_TO_UNICODE(pName));
#endif
	};
	gpuCallbacks.OnEventEnd = [](const char* pName, ID3D12GraphicsCommandList* pCmd, uint16 queueIndex)
	{
		gCPUProfiler.PopRegion();
#if ENABLE_PIX
		::PIXEndEvent(pCmd);
#endif
	};
	gGPUProfiler.RegisterEventCallback(gpuCallbacks);

#if ENABLE_PIX
	CPUProfilerCallbacks cpuCallbacks;
	cpuCallbacks.OnEventBegin = [](const char* pName, uint32 threadIndex) { ::PIXBeginEvent(0, MULTIBYTE_TO_UNICODE(pName)); };
	cpuCallbacks.OnEventEnd = [](const char* pName, uint32 threadIndex) { ::PIXEndEvent(); };
	gCPUProfiler.RegisterEventCallbacks(cpuCallbacks);
#endif
}

void App::Init_Internal()
{
	LIVE_PP();

#ifdef _DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#if BREAK_ON_ALLOC > 0
	_CrtSetBreakAlloc(BREAK_ON_ALLOC);
#endif
#endif

	Thread::SetMainThread();
	CommandLine::Parse(GetCommandLineA());

	if (CommandLine::GetBool("debuggerwait"))
	{
		while (!::IsDebuggerPresent())
		{
			::Sleep(100);
		}
	}

	Console::Initialize();
	ConsoleManager::Initialize();

	PROFILE_REGISTER_THREAD("Main Thread");
	TaskQueue::Initialize(std::thread::hardware_concurrency());

	Vector2i displayDimensions = Window::GetDisplaySize();

	m_Window.Init((int)(displayDimensions.x * 0.7f), (int)(displayDimensions.y * 0.7f));
	m_Window.OnKeyInput += [](uint32 character, bool isDown) { Input::Instance().UpdateKey(character, isDown); };
	m_Window.OnMouseInput += [](uint32 mouse, bool isDown) { Input::Instance().UpdateMouseKey(mouse, isDown); };
	m_Window.OnMouseMove += [](uint32 x, uint32 y) { Input::Instance().UpdateMousePosition((float)x, (float)y); };
	m_Window.OnMouseScroll += [](float wheel) { Input::Instance().UpdateMouseWheel(wheel); };
	m_Window.OnResizeOrMove += [this](uint32 width, uint32 height) { OnWindowResized_Internal(width, height); };

	Time::Reset();

	E_LOG(Info, "Graphics::InitD3D()");

	GraphicsDeviceOptions options;
	options.UseDebugDevice = CommandLine::GetBool("d3ddebug");
	options.UseDRED = CommandLine::GetBool("dred");
	options.LoadPIX = CommandLine::GetBool("pix");
	options.UseGPUValidation = CommandLine::GetBool("gpuvalidation");
	options.UseWarp = CommandLine::GetBool("warp");
	options.UseStablePowerState = CommandLine::GetBool("stablepowerstate");
	m_pDevice = new GraphicsDevice(options);

	InitializeProfiler(m_pDevice);

	m_pSwapchain = new SwapChain(m_pDevice, DisplayMode::SDR, 3, m_Window.GetNativeWindow());

	GraphicsCommon::Create(m_pDevice);

	ImGuiRenderer::Initialize(m_pDevice, m_Window.GetNativeWindow());

	Init();
}

void App::Update_Internal()
{
	Time::Tick();
	Input::Instance().Update();
	ImGuiRenderer::NewFrame();

	Update();

	{
		PROFILE_SCOPE("Execute Commandlist");
		CommandContext* pContext = m_pDevice->AllocateCommandContext();
		ImGuiRenderer::Render(*pContext, m_pSwapchain->GetBackBuffer());
		pContext->InsertResourceBarrier(m_pSwapchain->GetBackBuffer(), D3D12_RESOURCE_STATE_PRESENT);
		pContext->Execute();
	}

	{
		PROFILE_SCOPE("Present");
		m_pSwapchain->Present();
		ImGuiRenderer::PresentViewports();
	}
	{
		PROFILE_SCOPE("Wait for GPU frame");
		m_pDevice->TickFrame();
	}
}

void App::Shutdown_Internal()
{
	Shutdown();

	m_pDevice->IdleGPU();
	gGPUProfiler.Shutdown();

	ImGuiRenderer::Shutdown();
	GraphicsCommon::Destroy();

	TaskQueue::Shutdown();
	Console::Shutdown();
}

void App::OnWindowResized_Internal(uint32 width, uint32 height)
{
	E_LOG(Info, "Window resized: %dx%d", width, height);
	m_pSwapchain->OnResizeOrMove(width, height);
}