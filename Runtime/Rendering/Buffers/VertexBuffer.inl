#pragma once

#include "Rendering/Buffers/VertexBuffer.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace
{
	std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDevice()
	{
		try
		{
			auto& driver = NLS::Render::Context::RequireLocatedDriver("VertexBuffer");
			return NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
		}
		catch (...)
		{
			return nullptr;
		}
	}
}

namespace NLS::Render::Buffers
{
	template <class T>
	inline VertexBuffer<T>::VertexBuffer(T* p_data, size_t p_elements)
	{
		NLS::Render::RHI::RHIBufferDesc desc;
		desc.size = p_elements * sizeof(T);
		desc.usage = NLS::Render::RHI::BufferUsageFlags::Vertex;
		desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
		desc.debugName = "VertexBuffer";

		auto device = GetExplicitDevice();
		if (device != nullptr)
		{
			m_explicitBuffer = device->CreateBuffer(desc, p_data);
		}
	}

	template<class T>
	inline VertexBuffer<T>::VertexBuffer(std::vector<T>& p_data) : VertexBuffer(p_data.data(), p_data.size())
	{
	}

	template<class T>
	inline VertexBuffer<T>::~VertexBuffer()
	{
		m_explicitBuffer.reset();
	}

	template <class T>
	inline void VertexBuffer<T>::Bind()
	{
		// In formal RHI, binding is handled at command buffer level
		// This is a no-op placeholder
	}

	template <class T>
	inline void VertexBuffer<T>::Unbind()
	{
		// In formal RHI, unbinding is handled at command buffer level
		// This is a no-op placeholder
	}

	template <class T>
	inline uint32_t VertexBuffer<T>::GetID()
	{
		return 0; // Formal RHI has no legacy buffer ID
	}
}
