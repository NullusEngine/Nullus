#include "Debug/Assertion.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Buffers/IndexBuffer.h"
#include "Rendering/RHI/Backends/OpenGL/Compat/ExplicitRHICompat.h"

namespace
{
	using Driver = NLS::Render::Context::Driver;

	Driver& RequireDriver()
	{
		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<Driver>(), "IndexBuffer requires an initialized Driver.");
		return NLS_SERVICE(Driver);
	}
}

NLS::Render::Buffers::IndexBuffer::IndexBuffer(unsigned int* p_data, size_t p_elements)
{
	auto& driver = RequireDriver();
	m_bufferResource = driver.CreateBufferResource(NLS::Render::RHI::BufferType::Index);
	if (const auto explicitDevice = driver.GetExplicitDevice(); explicitDevice != nullptr)
	{
		NLS::Render::RHI::RHIBufferDesc desc;
		desc.size = p_elements * sizeof(unsigned int);
		desc.usage = NLS::Render::RHI::BufferUsageFlags::Index;
		desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
		desc.debugName = "IndexBuffer";
		m_explicitBuffer = explicitDevice->CreateBuffer(desc, p_data);
	}
	else
	{
		m_explicitBuffer = m_bufferResource
			? NLS::Render::RHI::WrapCompatibilityBuffer(m_bufferResource, "IndexBuffer")
			: nullptr;
	}
	m_bufferID = m_bufferResource ? m_bufferResource->GetResourceId() : 0;
	driver.BindBuffer(NLS::Render::RHI::BufferType::Index, m_bufferID);
	driver.SetBufferData(NLS::Render::RHI::BufferType::Index, p_elements * sizeof(unsigned int), p_data, NLS::Render::RHI::BufferUsage::StaticDraw);
}

NLS::Render::Buffers::IndexBuffer::IndexBuffer(std::vector<uint32_t>& p_data) : IndexBuffer(p_data.data(), p_data.size())
{
}

NLS::Render::Buffers::IndexBuffer::~IndexBuffer()
{
	if (m_bufferResource)
		m_bufferResource.reset();
	else if (m_bufferID != 0)
		RequireDriver().DestroyBuffer(m_bufferID);

	m_explicitBuffer.reset();
}

void NLS::Render::Buffers::IndexBuffer::Bind()
{
	RequireDriver().BindBuffer(NLS::Render::RHI::BufferType::Index, m_bufferID);
}

void NLS::Render::Buffers::IndexBuffer::Unbind()
{
	RequireDriver().BindBuffer(NLS::Render::RHI::BufferType::Index, 0);
}

uint32_t NLS::Render::Buffers::IndexBuffer::GetID()
{
	return m_bufferID;
}
