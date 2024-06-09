#pragma once

#include <cstdint>

namespace NLS::Rendering::Settings
{
	/**
	* Graphics operations (for stencil/depth buffers)
	*/
	enum class EOperation : uint8_t
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