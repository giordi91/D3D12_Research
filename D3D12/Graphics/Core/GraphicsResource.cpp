#include "stdafx.h"
#include "GraphicsResource.h"
#include "ResourceViews.h"

GraphicsResource::GraphicsResource(Graphics* pParent) 
	: GraphicsObject(pParent), m_pResource(nullptr), m_ResourceState(D3D12_RESOURCE_STATE_COMMON)
{
}

GraphicsResource::GraphicsResource(Graphics* pParent, ID3D12Resource* pResource, D3D12_RESOURCE_STATES state)
	: GraphicsObject(pParent), m_pResource(pResource), m_ResourceState(state)
{
}

GraphicsResource::~GraphicsResource()
{
	Release();
}

void GraphicsResource::Release()
{
	if (m_pResource)
	{
		m_pResource->Release();
		m_pResource = nullptr;
	}
}

void GraphicsResource::SetName(const char* pName)
{
	D3D::SetObjectName(m_pResource, pName);
}

std::string GraphicsResource::GetName() const
{
	if (m_pResource)
	{
		uint32 size = 0;
		m_pResource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, nullptr);
		std::string str(size, '\0');
		m_pResource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, &str[0]);
		return str;
	}
	return "";
}