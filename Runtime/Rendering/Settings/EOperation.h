#pragma once

#include <cstdint>
#include "RenderDef.h"
namespace NLS::Rendering::Settings
{
	/**
	* Graphics operations (for stencil/depth buffers)
	*/
	enum class NLS_RENDER_API EOperation : uint8_t
	{
		ZERO,
		KEEP,
		REPLACE,
		INCREMENT,
		INCREMENT_WRAP,
		DECREMENT,
		DECREMENT_WRAP,
		INVERT
	};
}