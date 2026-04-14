#include "Rendering/Buffers/UniformBuffer.h"
#include "Debug/Assertion.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

#include <cstring>

namespace NLS::Render::Buffers
{
namespace
{
	void UpdateShadowData(std::vector<uint8_t>& shadowData, size_t bufferSize, const void* data, uint32_t size, size_t offset)
	{
		if (data == nullptr || size == 0)
			return;

		NLS_ASSERT(offset + size <= bufferSize, "UniformBuffer shadow update exceeds allocated range.");
		if (shadowData.size() < bufferSize)
			shadowData.resize(bufferSize, 0u);

		std::memcpy(shadowData.data() + offset, data, size);
	}

	std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDevice()
	{
		try
		{
			auto& driver = NLS::Render::Context::RequireLocatedDriver("UniformBuffer");
			return NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
		}
		catch (...)
		{
			return nullptr;
		}
	}
}

UniformBuffer::UniformBuffer(
    size_t p_size,
    uint32_t p_bindingPoint,
    uint32_t p_offset,
    Settings::EAccessSpecifier p_accessSpecifier)
    : m_bindingPoint(p_bindingPoint)
{
    (void)p_offset;
    (void)p_accessSpecifier;
    m_size = p_size;
    m_shadowData.resize(m_size, 0u);
    NLS::Render::RHI::RHIBufferDesc desc;
    desc.size = p_size;
    desc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
    desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    desc.debugName = "UniformBuffer";

    auto device = GetExplicitDevice();
    if (device != nullptr)
    {
        m_explicitBuffer = device->CreateBuffer(desc, nullptr);
    }
}

UniformBuffer::~UniformBuffer()
{
    m_explicitBuffer.reset();
}

void UniformBuffer::Bind(uint32_t p_bindingPoint)
{
    // In formal RHI, binding is handled at command buffer level through descriptor sets
    // This is a no-op placeholder
    (void)p_bindingPoint;
}

void UniformBuffer::Unbind()
{
    // In formal RHI, unbinding is handled at command buffer level
    // This is a no-op placeholder
}

uint32_t UniformBuffer::GetID() const
{
    return 0; // Formal RHI has no legacy buffer ID
}

void UniformBuffer::_SetSubData(const void* p_data, uint32_t size, size_t p_offset)
{
    UpdateShadowData(m_shadowData, m_size, p_data, size, p_offset);
    // Note: m_explicitBuffer updates would need proper GPU upload mechanism (staging buffer)
}

void UniformBuffer::_SetSubData(const void* p_data, uint32_t size, std::reference_wrapper<size_t> p_offsetInOut)
{
    size_t dataSize = size;
    UpdateShadowData(m_shadowData, m_size, p_data, static_cast<uint32_t>(dataSize), p_offsetInOut.get());
    p_offsetInOut.get() += dataSize;
}

void UniformBuffer::SetRawData(const void* p_data, uint32_t size, size_t p_offset)
{
    _SetSubData(p_data, size, p_offset);
}

std::shared_ptr<NLS::Render::RHI::RHIBuffer> UniformBuffer::CreateExplicitSnapshotBuffer(const std::string& debugName) const
{
    if (m_size == 0)
        return nullptr;

    NLS::Render::RHI::RHIBufferDesc desc;
    desc.size = m_size;
    desc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
    desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    desc.debugName = debugName.empty() ? "UniformBufferSnapshot" : debugName;

    auto device = GetExplicitDevice();
    if (device != nullptr)
    {
        return device->CreateBuffer(desc, m_shadowData.empty() ? nullptr : m_shadowData.data());
    }
    return nullptr;
}
}
