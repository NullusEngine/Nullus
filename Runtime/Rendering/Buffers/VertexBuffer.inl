#pragma once

#include "Rendering/Buffers/VertexBuffer.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace
{
	std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDevice()
	{
		auto& driver = NLS::Render::Context::RequireLocatedDriver("VertexBuffer");
		return NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	}
}

namespace NLS::Render::Buffers
{
	template <class T>
	inline VertexBuffer<T>::VertexBuffer(const T* p_data, size_t p_elements)
	{
		if (p_data == nullptr || p_elements == 0u)
			return;

		NLS::Render::RHI::RHIBufferDesc desc;
		desc.size = p_elements * sizeof(T);
		desc.usage = NLS::Render::RHI::BufferUsageFlags::Vertex;
		desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
		desc.debugName = "VertexBuffer";

		auto device = GetExplicitDevice();
		if (device != nullptr)
		{
			NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
			uploadDesc.data = p_data;
			uploadDesc.dataSize = desc.size;
			uploadDesc.debugName = "VertexBufferInitialUpload";
			m_explicitBuffer = device->CreateBuffer(desc, uploadDesc);
		}
	}

	template<class T>
	inline VertexBuffer<T>::VertexBuffer(const std::vector<T>& p_data) : VertexBuffer(p_data.data(), p_data.size())
	{
	}

	template<class T>
	inline VertexBuffer<T>::~VertexBuffer()
	{
		m_explicitBuffer.reset();
	}
}
