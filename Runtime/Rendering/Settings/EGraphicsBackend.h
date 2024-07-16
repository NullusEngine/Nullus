#pragma once

#include <cstdint>
#include "RenderDef.h"
namespace NLS::Render::Settings
{
	/**
	* Enumerate graphics backend implementations
	*/
	enum class NLS_RENDER_API EGraphicsBackend : uint8_t
	{
		NONE,
		OPENGL,
		VULKAN
	};
}