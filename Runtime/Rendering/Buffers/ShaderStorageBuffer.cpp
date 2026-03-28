#include "Rendering/Buffers/ShaderStorageBuffer.h"
#include "Debug/Assertion.h"
#include "Core/ServiceLocator.h"
#include "Rendering/RHI/Backends/OpenGL/Compat/ExplicitRHICompat.h"
namespace
{
	using Driver = NLS::Render::Context::Driver;

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

	Driver& RequireDriver()
	{
		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<Driver>(), "ShaderStorageBuffer requires an initialized Driver.");
		return NLS_SERVICE(Driver);
	}
}

NLS::Render::Buffers::ShaderStorageBuffer::ShaderStorageBuffer(Settings::EAccessSpecifier p_accessSpecifier)
{
	auto& driver = RequireDriver();
	m_bufferResource = driver.CreateBufferResource(NLS::Render::RHI::BufferType::ShaderStorage);
	m_explicitBuffer = m_bufferResource
		? NLS::Render::RHI::WrapCompatibilityBuffer(m_bufferResource, "ShaderStorageBuffer")
		: nullptr;
	m_bufferID = m_bufferResource ? m_bufferResource->GetResourceId() : 0;
	driver.BindBuffer(NLS::Render::RHI::BufferType::ShaderStorage, m_bufferID);
	driver.SetBufferData(
		NLS::Render::RHI::BufferType::ShaderStorage,
		0,
		nullptr,
		ToRHIBufferUsage(p_accessSpecifier));
	driver.BindBuffer(NLS::Render::RHI::BufferType::ShaderStorage, 0);
}

NLS::Render::Buffers::ShaderStorageBuffer::~ShaderStorageBuffer()
{
	if (m_bufferResource)
		m_bufferResource.reset();
	else if (m_bufferID != 0)
		RequireDriver().DestroyBuffer(m_bufferID);

	m_explicitBuffer.reset();
}

void NLS::Render::Buffers::ShaderStorageBuffer::Bind(uint32_t p_bindingPoint)
{
	RequireDriver().BindBufferBase(NLS::Render::RHI::BufferType::ShaderStorage, p_bindingPoint, m_bufferID);
}

void NLS::Render::Buffers::ShaderStorageBuffer::Unbind()
{
	RequireDriver().BindBuffer(NLS::Render::RHI::BufferType::ShaderStorage, 0);
}
