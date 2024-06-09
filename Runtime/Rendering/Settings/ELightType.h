#pragma once

#include <cstdint>

namespace NLS::Rendering::Settings
{
	/**
	* Light types
	*/
	enum class ELightType : uint8_t
	{
		POINT,
		DIRECTIONAL,
		SPOT,
		AMBIENT_BOX,
		AMBIENT_SPHERE
	};
}