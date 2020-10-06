#include "stdafx.h"
#include "CommandQueue.h"
#include "Graphics.h"
#include "pix3.h"
#include "CommandContext.h"

CommandAllocatorPool::CommandAllocatorPool(Graphics* pGraphics, D3D12_COMMAND_LIST_TYPE type)
	: GraphicsObject(pGraphics), m_Type(type)
{
}

ID3D12CommandAllocator* CommandAllocatorPool::GetAllocator(uint64 fenceValue)
{
	std::scoped_lock<std::mutex> lock(m_AllocationMutex);
	if (m_FreeAllocators.empty() == false)
	{
		std::pair<ID3D12CommandAllocator*, uint64>& pFirst = m_FreeAllocators.front();
		if (pFirst.second <= fenceValue)
		{
			m_FreeAllocators.pop();
			pFirst.first->Reset();
			return pFirst.first;
		}
	}

	ComPtr<ID3D12CommandAllocator> pAllocator;
	m_pGraphics->GetDevice()->CreateCommandAllocator(m_Type, IID_PPV_ARGS(pAllocator.GetAddressOf()));
	D3D::SetObjectName(pAllocator.Get(), "Pooled Allocator");
	m_CommandAllocators.push_back(std::move(pAllocator));
	return m_CommandAllocators.back().Get();
}

void CommandAllocatorPool::FreeAllocator(ID3D12CommandAllocator* pAllocator, uint64 fenceValue)
{
	std::scoped_lock<std::mutex> lock(m_AllocationMutex);
	m_FreeAllocators.push(std::pair<ID3D12CommandAllocator*, uint64>(pAllocator, fenceValue));
}

CommandQueue::CommandQueue(Graphics* pGraphics, D3D12_COMMAND_LIST_TYPE type)
	: GraphicsObject(pGraphics),
	m_NextFenceValue((uint64)type << 56 | 1),
	m_LastCompletedFenceValue((uint64)type << 56),
	m_Type(type)
{
	m_pAllocatorPool = std::make_unique<CommandAllocatorPool>(pGraphics, type);

	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 0;
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	desc.Type = type;

	VERIFY_HR_EX(pGraphics->GetDevice()->CreateCommandQueue(&desc, IID_PPV_ARGS(m_pCommandQueue.GetAddressOf())), m_pGraphics->GetDevice());
	D3D::SetObjectName(m_pCommandQueue.Get(), "Main CommandQueue");
	VERIFY_HR_EX(pGraphics->GetDevice()->CreateFence(m_LastCompletedFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_pFence.GetAddressOf())), m_pGraphics->GetDevice());
	D3D::SetObjectName(m_pCommandQueue.Get(), "CommandQueue Fence");

	m_pFenceEventHandle = CreateEventExA(nullptr, "CommandQueue Fence", 0, EVENT_ALL_ACCESS);
}

CommandQueue::~CommandQueue()
{
	CloseHandle(m_pFenceEventHandle);
}

uint64 CommandQueue::ExecuteCommandLists(CommandContext** pCommandContexts, uint32 numContexts)
{
	check(pCommandContexts);
	check(numContexts > 0);

	// Commandlists can be recorded in parallel.
	// The before state of a resource transition can't be known so commandlists keep local resource states
	// and insert "pending resource barriers" which are barriers with an unknown before state.
	// During commandlist execution, these pending resource barriers are resolved by inserting
	// new barriers in the previous commandlist before closing it.
	// The first commandlist will resolve the barriers of the next so the first one will just contain resource barriers.

	std::vector<ID3D12CommandList*> commandLists;
	commandLists.reserve(numContexts + 1);
	CommandContext* pCurrentContext = nullptr;
	for (uint32 i = 0; i < numContexts; ++i)
	{
		CommandContext* pNextContext = pCommandContexts[i];
		check(pNextContext);

		ResourceBarrierBatcher barriers;
		for (const CommandContext::PendingBarrier& pending : pNextContext->GetPendingBarriers())
		{
			uint32 subResource = pending.Subresource;
			GraphicsResource* pResource = pending.pResource;
			D3D12_RESOURCE_STATES beforeState = pResource->GetResourceState(subResource);
			checkf(CommandContext::IsTransitionAllowed(m_Type, beforeState), 
				"Resource (%s) can not be transitioned from this state (%s) on this queue (%s). Insert a barrier on another queue before executing this one.", 
				pResource->GetName().c_str(), D3D::ResourceStateToString(beforeState).c_str(), D3D::CommandlistTypeToString(m_Type));
			barriers.AddTransition(pResource->GetResource(), beforeState, pending.State.Get(subResource), subResource);
			pResource->SetResourceState(pNextContext->GetResourceState(pending.pResource, subResource));
		}
		if (barriers.HasWork())
		{
			if (!pCurrentContext)
			{
				pCurrentContext = m_pGraphics->AllocateCommandContext(m_Type);
				pCurrentContext->Free(m_NextFenceValue);
			}
			barriers.Flush(pCurrentContext->GetCommandList());
		}
		if (pCurrentContext)
		{
			VERIFY_HR_EX(pCurrentContext->GetCommandList()->Close(), m_pGraphics->GetDevice());
			commandLists.push_back(pCurrentContext->GetCommandList());
		}
		pCurrentContext = pNextContext;
	}
	VERIFY_HR_EX(pCurrentContext->GetCommandList()->Close(), m_pGraphics->GetDevice());
	commandLists.push_back(pCurrentContext->GetCommandList());

	m_pCommandQueue->ExecuteCommandLists((uint32)commandLists.size(), commandLists.data());
	
	std::lock_guard<std::mutex> lock(m_FenceMutex);
	m_pCommandQueue->Signal(m_pFence.Get(), m_NextFenceValue);

	return m_NextFenceValue++;
}

bool CommandQueue::IsFenceComplete(uint64 fenceValue)
{
	if (fenceValue > m_LastCompletedFenceValue)
	{
		m_LastCompletedFenceValue = std::max(m_LastCompletedFenceValue, m_pFence->GetCompletedValue());
	}

	return fenceValue <= m_LastCompletedFenceValue;
}

void CommandQueue::InsertWaitForFence(uint64 fenceValue)
{
	CommandQueue* pFenceValueOwner = m_pGraphics->GetCommandQueue((D3D12_COMMAND_LIST_TYPE)(fenceValue >> 56));
	m_pCommandQueue->Wait(pFenceValueOwner->GetFence(), fenceValue);
}

void CommandQueue::InsertWaitForQueue(CommandQueue* pQueue)
{
	m_pCommandQueue->Wait(pQueue->GetFence(), pQueue->GetNextFenceValue() - 1);
}

uint64 CommandQueue::IncrementFence()
{
	std::lock_guard<std::mutex> LockGuard(m_FenceMutex);
	m_pCommandQueue->Signal(m_pFence.Get(), m_NextFenceValue);
	return m_NextFenceValue++;
}

ID3D12CommandAllocator* CommandQueue::RequestAllocator()
{
	uint64 completedFence = m_pFence->GetCompletedValue();
	return m_pAllocatorPool->GetAllocator(completedFence);
}

void CommandQueue::FreeAllocator(uint64 fenceValue, ID3D12CommandAllocator* pAllocator)
{
	m_pAllocatorPool->FreeAllocator(pAllocator, fenceValue);
}

void CommandQueue::WaitForFence(uint64 fenceValue)
{
	if (IsFenceComplete(fenceValue))
	{
		return;
	}

	std::lock_guard<std::mutex> lockGuard(m_EventMutex);

	m_pFence->SetEventOnCompletion(fenceValue, m_pFenceEventHandle);
	DWORD result = WaitForSingleObject(m_pFenceEventHandle, INFINITE);

	// The event was successfully signaled, so notify PIX
	if(result == WAIT_OBJECT_0)
	{
		PIXNotifyWakeFromFenceSignal(m_pFenceEventHandle);
	}

	m_LastCompletedFenceValue = fenceValue;
}

void CommandQueue::WaitForIdle()
{
	WaitForFence(IncrementFence());
}