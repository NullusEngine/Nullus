#pragma once

#include <cstdint>

namespace NLS::Rendering::Settings
{
	/**
	* Comparaison algorithm used by depth/stencil operations
	*/
	enum class EComparaisonAlgorithm : uint8_t
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
