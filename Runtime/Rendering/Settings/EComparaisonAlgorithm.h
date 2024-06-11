#pragma once

#include <cstdint>
#include "RenderDef.h"
namespace NLS::Rendering::Settings
{
	/**
	* Comparaison algorithm used by depth/stencil operations
	*/
	enum class NLS_RENDER_API EComparaisonAlgorithm : uint8_t
	{
		NEVER,
		LESS,
		EQUAL,
		LESS_EQUAL,
		GREATER,
		NOTEQUAL,
		GREATER_EQUAL,
		ALWAYS
	};
}
