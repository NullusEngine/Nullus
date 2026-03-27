#pragma once
#include "Rendering/Buffers/VertexArray.h"

namespace NLS::Render::Buffers
{
	template <class T>
	inline void VertexArray::BindAttribute(
		uint32_t p_attribute,
		VertexBuffer<T>& p_vertexBuffer,
		Settings::EDataType p_type,
		uint64_t p_count,
		uint64_t p_stride,
		intptr_t p_offset
	) const
	{
		(void)p_attribute;
		(void)p_vertexBuffer;
		(void)p_type;
		(void)p_count;
		(void)p_stride;
		(void)p_offset;
	}
}
