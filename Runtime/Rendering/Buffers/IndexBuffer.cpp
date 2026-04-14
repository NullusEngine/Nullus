#include "Rendering/Buffers/IndexBuffer.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace
{
	std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDevice()
	{
		try
		{
			auto& driver = NLS::Render::Context::RequireLocatedDriver("IndexBuffer");
			return NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
		}
		catch (...)
		{
			return nullptr;
		}
	}
}

NLS::Render::Buffers::IndexBuffer::IndexBuffer(unsigned int* p_data, size_t p_elements)
{
	NLS::Render::RHI::RHIBufferDesc desc;
	desc.size = p_elements * sizeof(unsigned int);
	desc.usage = NLS::Render::RHI::BufferUsageFlags::Index;
	desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
	desc.debugName = "IndexBuffer";

	auto device = GetExplicitDevice();
	if (device != nullptr)
	{
		m_explicitBuffer = device->CreateBuffer(desc, p_data);
	}
}

NLS::Render::Buffers::IndexBuffer::IndexBuffer(std::vector<uint32_t>& p_data) : IndexBuffer(p_data.data(), p_data.size())
{
}

NLS::Render::Buffers::IndexBuffer::~IndexBuffer()
{
	m_explicitBuffer.reset();
}

void NLS::Render::Buffers::IndexBuffer::Bind()
{
	// In formal RHI, binding is handled at command buffer level
	// This is a no-op placeholder
}

void NLS::Render::Buffers::IndexBuffer::Unbind()
{
	// In formal RHI, unbinding is handled at command buffer level
	// This is a no-op placeholder
}

uint32_t NLS::Render::Buffers::IndexBuffer::GetID()
{
	// In formal RHI, there's no buffer ID - return 0 as placeholder
	return 0;
}
