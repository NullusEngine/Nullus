#pragma once

#include "Rendering/Buffers/ShaderStorageBuffer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace
{
	std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDeviceForSSBO()
	{
		auto& driver = NLS::Render::Context::RequireLocatedDriver("ShaderStorageBuffer");
		return NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	}
}

namespace NLS::Render::Buffers
{
	template<typename T>
	inline void ShaderStorageBuffer::SendBlocks(T* p_data, size_t p_size)
	{
		if (p_data == nullptr || p_size == 0)
			return;

		auto device = GetExplicitDeviceForSSBO();
		if (device == nullptr)
			return;

		m_explicitBuffer.reset();

		NLS::Render::RHI::RHIBufferDesc desc;
		desc.size = p_size;
		desc.usage = NLS::Render::RHI::BufferUsageFlags::Storage;
		desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
		desc.debugName = "ShaderStorageBuffer";

		m_explicitBuffer = device->CreateBuffer(desc, p_data);
		m_currentSize = p_size;
	}
}
