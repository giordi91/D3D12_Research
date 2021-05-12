#include "stdafx.h"
#include "Console.h"
#include "CommandLine.h"

#if PLATFORM_WINDOWS
namespace Win32Console
{
	static HANDLE Open()
	{
		HANDLE handle = NULL;
		if (AllocConsole())
		{
			// Redirect the CRT standard input, output, and error handles to the console
			FILE* pCout;
			freopen_s(&pCout, "CONIN$", "r", stdin);
			freopen_s(&pCout, "CONOUT$", "w", stdout);
			freopen_s(&pCout, "CONOUT$", "w", stderr);

			//Clear the error state for each of the C++ standard stream objects. We need to do this, as
			//attempts to access the standard streams before they refer to a valid target will cause the
			//iostream objects to enter an error state. In versions of Visual Studio after 2005, this seems
			//to always occur during startup regardless of whether anything has been read from or written to
			//the console or not.
			std::wcout.clear();
			std::cout.clear();
			std::wcerr.clear();
			std::cerr.clear();
			std::wcin.clear();
			std::cin.clear();

			handle = GetStdHandle(STD_OUTPUT_HANDLE);

			//Disable Close-Button
			HWND hwnd = GetConsoleWindow();
			if (hwnd != nullptr)
			{
				HMENU hMenu = GetSystemMenu(hwnd, FALSE);
				if (hMenu != nullptr)
				{
					DeleteMenu(hMenu, SC_CLOSE, MF_BYCOMMAND);
				}
			}
		}
		return handle;;
	}

	static void Close(HANDLE handle)
	{
		if(handle)
			CloseHandle(handle);
	}
};
#endif

static HANDLE sConsoleHandle = nullptr;
static std::mutex sLogMutex;
static std::queue<Console::LogEntry> sMessageQueue;
static LogType sVerbosity;
static std::deque<Console::LogEntry> sHistory;

void Console::Initialize()
{
#if WITH_CONSOLE
	if (CommandLine::GetBool("noconsole") == false)
	{
#if PLATFORM_WINDOWS
		sConsoleHandle = Win32Console::Open();
#endif
	}
#endif
	E_LOG(Info, "Startup");
}

void Console::Shutdown()
{
	Win32Console::Close(sConsoleHandle);
}

void Console::Log(const char* message, LogType type)
{
	if ((int)type < (int)sVerbosity)
	{
		return;
	}

	const char* pVerbosityMessage = "";
	switch (type)
	{
	case LogType::Info:
		if (sConsoleHandle)
			SetConsoleTextAttribute(sConsoleHandle, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
		pVerbosityMessage = "[INFO]";
		break;
	case LogType::Warning:
		if (sConsoleHandle)
			SetConsoleTextAttribute(sConsoleHandle, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
		pVerbosityMessage = "[WARNING]";
		break;
	case LogType::Error:
	case LogType::FatalError:
		if (sConsoleHandle)
			SetConsoleTextAttribute(sConsoleHandle, FOREGROUND_RED | FOREGROUND_INTENSITY);
		pVerbosityMessage = "[ERROR]";
		break;
	default:
		break;
	}

	char messageBuffer[4096];
	stbsp_sprintf(messageBuffer, "%s %s\n", pVerbosityMessage, message);
	printf("%s %s\n", pVerbosityMessage, message);
	OutputDebugStringA(messageBuffer);

	if (sConsoleHandle)
	{
		SetConsoleTextAttribute(sConsoleHandle, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
	}

	if (Thread::IsMainThread())
	{
		{
			std::scoped_lock<std::mutex> lock(sLogMutex);
			while (!sMessageQueue.empty())
			{
				const LogEntry& e = sMessageQueue.front();
				sHistory.push_back(e);
				sMessageQueue.pop();
			}
		}

		sHistory.push_back(LogEntry(message, type));
		while (sHistory.size() > 50)
		{
			sHistory.pop_front();
		}
	}
	else
	{
		std::scoped_lock<std::mutex> lock(sLogMutex);
		sMessageQueue.push(LogEntry(message, type));
	}

	if (type == LogType::Error)
	{
		__debugbreak();
	}
	else if (type == LogType::FatalError)
	{
		abort();
	}
}

void Console::LogFormat(LogType type, const char* format, ...)
{
	static char sConvertBuffer[8196];
	va_list ap;
	va_start(ap, format);
	vsnprintf_s(sConvertBuffer, 8196, format, ap);
	va_end(ap);
	Log(sConvertBuffer, type);
}

void Console::SetVerbosity(LogType type)
{
	sVerbosity = type;
}

const std::deque<Console::LogEntry>& Console::GetHistory()
{
	return sHistory;
}
