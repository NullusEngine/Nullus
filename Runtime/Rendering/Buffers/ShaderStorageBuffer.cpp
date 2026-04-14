#include "Rendering/Buffers/ShaderStorageBuffer.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace NLS::Render::Buffers
{
	NLS::Render::RHI::BufferUsage ToRHIBufferUsage(NLS::Render::Settings::EAccessSpecifier accessSpecifier)
	{
		switch (accessSpecifier)
		{
		case NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW: return NLS::Render::RHI::BufferUsage::StreamDraw;
		case NLS::Render::Settings::EAccessSpecifier::STATIC_DRAW: return NLS::Render::RHI::BufferUsage::StaticDraw;
		case NLS::Render::Settings::EAccessSpecifier::DYNAMIC_DRAW:
		default:
			return NLS::Render::RHI::BufferUsage::DynamicDraw;
		}
	}
}

NLS::Render::Buffers::ShaderStorageBuffer::ShaderStorageBuffer(Settings::EAccessSpecifier p_accessSpecifier)
{
	NLS::Render::RHI::RHIBufferDesc desc;
	desc.size = 0; // Will be set when SendBlocks is called
	desc.usage = NLS::Render::RHI::BufferUsageFlags::Storage;
	desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
	desc.debugName = "ShaderStorageBuffer";

	auto device = GetExplicitDeviceForSSBO();
	if (device != nullptr)
	{
		m_explicitBuffer = device->CreateBuffer(desc, nullptr);
	}
}

NLS::Render::Buffers::ShaderStorageBuffer::~ShaderStorageBuffer()
{
	m_explicitBuffer.reset();
}

void NLS::Render::Buffers::ShaderStorageBuffer::Bind(uint32_t p_bindingPoint)
{
	// In formal RHI, binding is handled at command buffer level through descriptor sets
	// This is a no-op placeholder
	(void)p_bindingPoint;
}

void NLS::Render::Buffers::ShaderStorageBuffer::Unbind()
{
	// In formal RHI, unbinding is handled at command buffer level
	// This is a no-op placeholder
}
