#pragma once

#include <glad/glad.h>

#include "Rendering/Buffers/UniformBuffer.h"

namespace NLS::Rendering::Buffers
{
	template<typename T>
	inline void UniformBuffer::SetSubData(const T& p_data, size_t p_offsetInOut)
	{
		// TODO: Maybe we could find a way to set sub data without having to use bind/unbind, would be more efficient
        _SetSubData(std::addressof(p_data), sizeof(T), p_offsetInOut);

	}

	template<typename T>
	inline void UniformBuffer::SetSubData(const T& p_data, std::reference_wrapper<size_t> p_offsetInOut)
	{
        _SetSubData(std::addressof(p_data), sizeof(T), p_offsetInOut);
	}
}