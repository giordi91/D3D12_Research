#pragma once

#define FOO_SCOPE(...) FooProfileScope MACRO_CONCAT(profiler, __COUNTER__)(__FUNCTION__, __FILE__, __LINE__, __VA_ARGS__)
#define FOO_REGISTER_THREAD(...) gProfiler.RegisterThread(__VA_ARGS__)
#define FOO_FRAME() gProfiler.Tick();

extern class FooProfiler gProfiler;

class FooProfiler
{
public:
	static constexpr int REGION_HISTORY = 4;
	static constexpr int MAX_DEPTH = 32;
	static constexpr int STRING_BUFFER_SIZE = 1 << 16;
	static constexpr int MAX_NUM_REGIONS = 1024;

	FooProfiler() = default;

	void BeginRegion(const char* pName, const Color& color = Color(0.8f, 0.8f, 0.8f))
	{
		if (m_Paused)
			return;

		SampleHistory& data = GetCurrentData();
		uint32 newIndex = data.CurrentIndex.fetch_add(1);
		check(newIndex < data.Regions.size());

		TLS& tls = GetTLS();
		check(tls.Depth < ARRAYSIZE(tls.RegionStack));

		SampleRegion& newRegion = data.Regions[newIndex];
		newRegion.Depth = tls.Depth;
		newRegion.ThreadIndex = tls.ThreadIndex;
		newRegion.pName = StoreString(pName);
		newRegion.Color = Math::Pack_RGBA8_UNORM(color);
		QueryPerformanceCounter((LARGE_INTEGER*)(&newRegion.BeginTicks));

		tls.RegionStack[tls.Depth] = newIndex;
		tls.Depth++;
	}

	void SetFileInfo(const char* pFilePath, uint32 lineNumber)
	{
		if (m_Paused)
			return;

		SampleHistory& data = GetCurrentData();
		TLS& tls = GetTLS();

		SampleRegion& region = data.Regions[tls.RegionStack[tls.Depth - 1]];
		region.pFilePath = pFilePath;
		region.LineNumber = lineNumber;
	}

	void EndRegion()
	{
		if (m_Paused)
			return;

		SampleHistory& data = GetCurrentData();
		TLS& tls = GetTLS();

		check(tls.Depth > 0);
		--tls.Depth;
		SampleRegion& region = data.Regions[tls.RegionStack[tls.Depth]];
		QueryPerformanceCounter((LARGE_INTEGER*)(&region.EndTicks));
	}

	void Tick()
	{
		if (m_Paused)
			return;
			
		QueryPerformanceCounter((LARGE_INTEGER*)(&GetCurrentData().TicksEnd));

		for (auto& threadData : m_ThreadData)
			check(threadData.pTLS->Depth == 0);

		++m_FrameIndex;
		SampleHistory& data = GetCurrentData();
		data.CharIndex = 0;
		data.CurrentIndex = 0;

		QueryPerformanceCounter((LARGE_INTEGER*)(&GetCurrentData().TicksBegin));
	}

	void RegisterThread(const char* pName = nullptr)
	{
		TLS& tls = GetTLSUnsafe();
		check(!tls.IsInitialized);
		tls.IsInitialized = true;
		std::scoped_lock lock(m_ThreadDataLock);
		tls.ThreadIndex = (uint32)m_ThreadData.size();
		ThreadData& data = m_ThreadData.emplace_back();
		if (pName)
		{
			data.Name = pName;
		}
		else
		{
			PWSTR pDescription = nullptr;
			GetThreadDescription(GetCurrentThread(), &pDescription);
			data.Name = UNICODE_TO_MULTIBYTE(pDescription);
		}
		data.ThreadID = GetCurrentThreadId();
		data.pTLS = &tls;
	}

	void DrawHUD();

private:
	const char* StoreString(const char* pText)
	{
		SampleHistory& data = GetCurrentData();
		uint32 len = (uint32)strlen(pText) + 1;
		uint32 offset = data.CharIndex.fetch_add(len);
		check(offset + len <= ARRAYSIZE(data.StringBuffer));
		strcpy_s(data.StringBuffer + offset, len, pText);
		return &data.StringBuffer[offset];
	}

	struct TLS
	{
		uint32 ThreadIndex = 0;
		uint32 Depth = 0;
		uint32 RegionStack[MAX_DEPTH];
		bool IsInitialized = false;
	};

	TLS& GetTLSUnsafe()
	{
		static thread_local TLS tls;
		return tls;
	}

	TLS& GetTLS()
	{
		TLS& tls = GetTLSUnsafe();
		if (!tls.IsInitialized)
			RegisterThread();
		return tls;
	}

	struct SampleRegion
	{
		const char* pName;									//< Name of the region
		uint32 ThreadIndex = 0xFFFFFFFF;					//< Thread Index of the thread that recorderd this region
		uint64 BeginTicks = 0;								//< The ticks at the start of this region
		uint64 EndTicks = 0;								//< The ticks at the end of this region
		uint32 Color = 0xFFFF00FF;							//< Color of region
		uint32 Depth = 0;									//< Depth of the region
		uint32 LineNumber = 0;								//< Line number of file in which this region is recorded
		const char* pFilePath = nullptr;					//< File path of file in which this region is recorded
	};

	struct SampleHistory
	{
		uint64 TicksBegin;									//< The start ticks of the frame on the main thread
		uint64 TicksEnd;									//< The end ticks of the frame on the main thread
		std::array<SampleRegion, MAX_NUM_REGIONS> Regions;	//< All sample regions of the frame
		std::atomic<uint32> CurrentIndex = 0;				//< The index to the next free sample region
		std::atomic<uint32> CharIndex = 0;					//< The index to the next free char buffer
		char StringBuffer[STRING_BUFFER_SIZE];				//< Blob to store dynamic strings for the frame
	};

	SampleHistory& GetCurrentData()
	{
		return m_SampleHistory[m_FrameIndex % m_SampleHistory.size()];
	}

	const SampleHistory& GetHistoryData(int numFrames) const
	{
		numFrames = Math::Min(numFrames, (int)m_SampleHistory.size() - 2);
		int index = (m_FrameIndex + (int)m_SampleHistory.size() - 1 - numFrames) % (int)m_SampleHistory.size();
		return m_SampleHistory[index];
	}

	struct ThreadData
	{
		std::string Name = "";
		uint32 ThreadID = 0;
		const TLS* pTLS = nullptr;
	};

	std::mutex m_ThreadDataLock;
	std::vector<ThreadData> m_ThreadData;

	bool m_Paused = false;
	uint32 m_FrameIndex = 0;
	std::array<SampleHistory, REGION_HISTORY> m_SampleHistory;
};

struct FooProfileScope
{
	FooProfileScope(const char* pFunctionName, const char* pFilePath, uint32 lineNumber, const char* pName, const Color& color)
	{
		gProfiler.BeginRegion(pName ? pName : pFunctionName, color);
		gProfiler.SetFileInfo(pFilePath, lineNumber);
	}

	FooProfileScope(const char* pFunctionName, const char* pFilePath, uint32 lineNumber, const char* pName = nullptr)
	{
		gProfiler.BeginRegion(pName ? pName : pFunctionName);
		gProfiler.SetFileInfo(pFilePath, lineNumber);
	}

	~FooProfileScope()
	{
		gProfiler.EndRegion();
	}
};
