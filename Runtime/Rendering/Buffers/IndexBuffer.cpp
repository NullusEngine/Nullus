#include "Rendering/Buffers/IndexBuffer.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace
{
	std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDevice()
	{
		auto& driver = NLS::Render::Context::RequireLocatedDriver("IndexBuffer");
		return NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	}
}

NLS::Render::Buffers::IndexBuffer::IndexBuffer(
	unsigned int* p_data,
	size_t p_elements,
	const NLS::Render::RHI::MemoryUsage memoryUsage)
{
	if (p_data == nullptr || p_elements == 0u)
		return;

	NLS::Render::RHI::RHIBufferDesc desc;
	desc.size = p_elements * sizeof(unsigned int);
	desc.usage = NLS::Render::RHI::BufferUsageFlags::Index;
	desc.memoryUsage = memoryUsage;
	desc.debugName = "IndexBuffer";

	auto device = GetExplicitDevice();
	if (device != nullptr)
	{
		NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
		uploadDesc.data = p_data;
		uploadDesc.dataSize = desc.size;
		uploadDesc.debugName = "IndexBufferInitialUpload";
		m_explicitBuffer = device->CreateBuffer(desc, uploadDesc);
	}
}

NLS::Render::Buffers::IndexBuffer::IndexBuffer(
	std::vector<uint32_t>& p_data,
	const NLS::Render::RHI::MemoryUsage memoryUsage) : IndexBuffer(p_data.data(), p_data.size(), memoryUsage)
{
}

NLS::Render::Buffers::IndexBuffer::~IndexBuffer()
{
	m_explicitBuffer.reset();
}

