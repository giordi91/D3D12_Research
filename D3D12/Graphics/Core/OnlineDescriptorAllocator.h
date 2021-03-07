#pragma once
#include "DescriptorHandle.h"
#include "GraphicsResource.h"
#include "Core/BitField.h"
#include "RootSignature.h"

class CommandContext;
class Graphics;
enum class CommandListContext;

struct DescriptorHeapBlock
{
	DescriptorHeapBlock(DescriptorHandle startHandle, uint32 size, uint32 currentOffset)
		: StartHandle(startHandle), Size(size), CurrentOffset(currentOffset), FenceValue(0)
	{}
	DescriptorHandle StartHandle;
	uint32 Size;
	uint32 CurrentOffset;
	uint64 FenceValue;
};

class GlobalOnlineDescriptorHeap : public GraphicsObject
{
public:
	GlobalOnlineDescriptorHeap(Graphics* pParent, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 blockSize, uint32 numDescriptors);

	DescriptorHeapBlock* AllocateBlock();
	void FreeBlock(uint64 fenceValue, DescriptorHeapBlock* pBlock);
	uint32 GetDescriptorSize() const { return m_DescriptorSize; }
	ID3D12DescriptorHeap* GetHeap() const { return m_pHeap.Get(); }
	D3D12_DESCRIPTOR_HEAP_TYPE GetType() const { return m_Type; }
	DescriptorHandle GetStartHandle() const { return m_StartHandle; }

private:
	std::mutex m_BlockAllocateMutex;
	D3D12_DESCRIPTOR_HEAP_TYPE m_Type;
	uint32 m_NumDescriptors;

	uint32 m_DescriptorSize = 0;
	DescriptorHandle m_StartHandle;

	ComPtr<ID3D12DescriptorHeap> m_pHeap;
	std::vector<std::unique_ptr<DescriptorHeapBlock>> m_HeapBlocks;
	std::vector<DescriptorHeapBlock*> m_ReleasedBlocks;
	std::queue<DescriptorHeapBlock*> m_FreeBlocks;
};

class OnlineDescriptorAllocator : public GraphicsObject
{
public:
	OnlineDescriptorAllocator(GlobalOnlineDescriptorHeap* pGlobalHeap, CommandContext* pContext);
	~OnlineDescriptorAllocator();

	DescriptorHandle Allocate(uint32 count);

	void SetDescriptors(uint32 rootIndex, uint32 offset, uint32 numHandles, const D3D12_CPU_DESCRIPTOR_HANDLE* pHandles);
	void BindStagedDescriptors(CommandListContext descriptorTableType);

	void ParseRootSignature(RootSignature* pRootSignature);
	void ReleaseUsedHeaps(uint64 fenceValue);

private:
	CommandContext* m_pOwner;
	D3D12_DESCRIPTOR_HEAP_TYPE m_Type;
	struct RootDescriptorEntry
	{
		uint32 TableSize = 0;
		DescriptorHandle Descriptor;
	};
	std::array<RootDescriptorEntry, MAX_NUM_ROOT_PARAMETERS> m_RootDescriptorTable = {};

	RootSignatureMask m_RootDescriptorMask {};
	RootSignatureMask m_StaleRootParameters {};

	GlobalOnlineDescriptorHeap* m_pHeapAllocator;
	DescriptorHeapBlock* m_pCurrentHeapBlock = nullptr;
	std::vector<DescriptorHeapBlock*> m_ReleasedBlocks;
};
