#pragma once

#include <cstdint>
#include "RenderDef.h"
namespace NLS::Rendering::Settings
{
	/**
	* Light types
	*/
	enum class NLS_RENDER_API ELightType : uint8_t
	{
		POINT,
		DIRECTIONAL,
		SPOT,
		AMBIENT_BOX,
		AMBIENT_SPHERE
	};
}