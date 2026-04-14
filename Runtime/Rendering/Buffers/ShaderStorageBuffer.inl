#pragma once

#include "Rendering/Buffers/ShaderStorageBuffer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace
{
	std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDeviceForSSBO()
	{
		try
		{
			auto& driver = NLS::Render::Context::RequireLocatedDriver("ShaderStorageBuffer");
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
	template<typename T>
	inline void ShaderStorageBuffer::SendBlocks(T* p_data, size_t p_size)
	{
		// Formal RHI path: recreate buffer if size changed or initial creation (size=0)
		if (m_explicitBuffer == nullptr)
			return;

		if (m_currentSize == p_size && p_size > 0)
			return; // Size unchanged and already initialized

		// Need to recreate buffer with new size
		auto device = GetExplicitDeviceForSSBO();
		if (device == nullptr)
			return;

		// Release old buffer
		m_explicitBuffer.reset();

		// Create new buffer with correct size and initial data
		NLS::Render::RHI::RHIBufferDesc desc;
		desc.size = p_size;
		desc.usage = NLS::Render::RHI::BufferUsageFlags::Storage;
		desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
		desc.debugName = "ShaderStorageBuffer";

		m_explicitBuffer = device->CreateBuffer(desc, p_data);
		m_currentSize = p_size;
	}
}
