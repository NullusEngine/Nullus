#pragma once

#include <vector>

#include "Rendering/Buffers/VertexArray.h"

namespace NLS::Render::Resources
{
	/**
	* Interface for any mesh
	*/
	class IMesh
	{
	public:
		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;
		virtual uint32_t GetVertexCount() const = 0;
		virtual uint32_t GetIndexCount() const = 0;
	};
}