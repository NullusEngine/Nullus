#include "Rendering/Buffers/UniformBuffer.h"
#include "Debug/Assertion.h"
#include "Core/ServiceLocator.h"
#include "Rendering/RHI/Backends/OpenGL/Compat/ExplicitRHICompat.h"

#include <cstring>

namespace NLS::Render::Buffers
{
using Driver = NLS::Render::Context::Driver;

namespace
{
	NLS::Render::RHI::BufferUsage ToRHIBufferUsage(Settings::EAccessSpecifier accessSpecifier)
	{
		switch (accessSpecifier)
		{
		case Settings::EAccessSpecifier::STREAM_DRAW: return NLS::Render::RHI::BufferUsage::StreamDraw;
		case Settings::EAccessSpecifier::STATIC_DRAW: return NLS::Render::RHI::BufferUsage::StaticDraw;
		case Settings::EAccessSpecifier::DYNAMIC_DRAW:
		default:
			return NLS::Render::RHI::BufferUsage::DynamicDraw;
		}
	}

	Driver& RequireDriver()
	{
		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<Driver>(), "UniformBuffer requires an initialized Driver.");
		return NLS_SERVICE(Driver);
	}

	void UpdateShadowData(std::vector<uint8_t>& shadowData, size_t bufferSize, const void* data, uint32_t size, size_t offset)
	{
		if (data == nullptr || size == 0)
			return;

		NLS_ASSERT(offset + size <= bufferSize, "UniformBuffer shadow update exceeds allocated range.");
		if (shadowData.size() < bufferSize)
			shadowData.resize(bufferSize, 0u);

		std::memcpy(shadowData.data() + offset, data, size);
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
    m_size = p_size;
    m_shadowData.resize(m_size, 0u);
    auto& driver = RequireDriver();
    m_bufferResource = driver.CreateBufferResource(NLS::Render::RHI::BufferType::Uniform);
    if (const auto explicitDevice = driver.GetExplicitDevice(); explicitDevice != nullptr)
    {
        NLS::Render::RHI::RHIBufferDesc desc;
        desc.size = p_size;
        desc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
        desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
        desc.debugName = "UniformBuffer";
        m_explicitBuffer = explicitDevice->CreateBuffer(desc, nullptr);
    }
    else
    {
        m_explicitBuffer = m_bufferResource
            ? NLS::Render::RHI::WrapCompatibilityBuffer(m_bufferResource, "UniformBuffer")
            : nullptr;
    }
    m_bufferID = m_bufferResource ? m_bufferResource->GetResourceId() : 0;
    driver.BindBuffer(NLS::Render::RHI::BufferType::Uniform, m_bufferID);
    driver.SetBufferData(
        NLS::Render::RHI::BufferType::Uniform,
        p_size,
        nullptr,
        ToRHIBufferUsage(p_accessSpecifier));
    driver.BindBuffer(NLS::Render::RHI::BufferType::Uniform, 0);
}

UniformBuffer::~UniformBuffer()
{
    if (m_bufferResource)
        m_bufferResource.reset();
    else if (m_bufferID != 0)
        RequireDriver().DestroyBuffer(m_bufferID);

    m_explicitBuffer.reset();
}

void UniformBuffer::Bind(uint32_t p_bindingPoint)
{
    RequireDriver().BindBufferBase(NLS::Render::RHI::BufferType::Uniform, p_bindingPoint, m_bufferID);
}

void UniformBuffer::Unbind()
{
    RequireDriver().BindBuffer(NLS::Render::RHI::BufferType::Uniform, 0);
}

uint32_t UniformBuffer::GetID() const
{
    return m_bufferID;
}

void UniformBuffer::_SetSubData(const void* p_data, uint32_t size, size_t p_offset)
{
    UpdateShadowData(m_shadowData, m_size, p_data, size, p_offset);
    auto& driver = RequireDriver();
    driver.BindBuffer(NLS::Render::RHI::BufferType::Uniform, m_bufferID);
    driver.SetBufferSubData(NLS::Render::RHI::BufferType::Uniform, p_offset, size, p_data);
    driver.BindBuffer(NLS::Render::RHI::BufferType::Uniform, 0);
}

void UniformBuffer::_SetSubData(const void* p_data, uint32_t size, std::reference_wrapper<size_t> p_offsetInOut)
{
    size_t dataSize = size;
    UpdateShadowData(m_shadowData, m_size, p_data, static_cast<uint32_t>(dataSize), p_offsetInOut.get());
    auto& driver = RequireDriver();
    driver.BindBuffer(NLS::Render::RHI::BufferType::Uniform, m_bufferID);
    driver.SetBufferSubData(NLS::Render::RHI::BufferType::Uniform, p_offsetInOut.get(), dataSize, p_data);
    driver.BindBuffer(NLS::Render::RHI::BufferType::Uniform, 0);
    p_offsetInOut.get() += dataSize;
}

void UniformBuffer::SetRawData(const void* p_data, uint32_t size, size_t p_offset)
{
    _SetSubData(p_data, size, p_offset);
}

std::shared_ptr<NLS::Render::RHI::RHIBuffer> UniformBuffer::CreateExplicitSnapshotBuffer(const std::string& debugName) const
{
    auto& driver = RequireDriver();
    const auto explicitDevice = driver.GetExplicitDevice();
    if (explicitDevice == nullptr || m_size == 0)
        return nullptr;

    NLS::Render::RHI::RHIBufferDesc desc;
    desc.size = m_size;
    desc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
    desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    desc.debugName = debugName.empty() ? "UniformBufferSnapshot" : debugName;
    return explicitDevice->CreateBuffer(desc, m_shadowData.empty() ? nullptr : m_shadowData.data());
}
}
