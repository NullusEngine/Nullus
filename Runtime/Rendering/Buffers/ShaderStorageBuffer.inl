#pragma once

#include "Rendering/Buffers/ShaderStorageBuffer.h"
#include "Debug/Assertion.h"
#include "Core/ServiceLocator.h"

namespace NLS::Render::Buffers
{
	template<typename T>
	inline void ShaderStorageBuffer::SendBlocks(T* p_data, size_t p_size)
	{
		using Driver = NLS::Render::Context::Driver;

		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<Driver>(), "ShaderStorageBuffer requires an initialized Driver.");
		auto& driver = NLS_SERVICE(Driver);
		driver.BindBuffer(NLS::Render::RHI::BufferType::ShaderStorage, m_bufferID);
		driver.SetBufferData(
			NLS::Render::RHI::BufferType::ShaderStorage,
			p_size,
			p_data,
			NLS::Render::RHI::BufferUsage::DynamicDraw);
		driver.BindBuffer(NLS::Render::RHI::BufferType::ShaderStorage, 0);
	}
}
