#pragma once

#include <cstdint>
#include "RenderDef.h"
#include "Reflection/Macros.h"
namespace NLS::Render::Settings
{
	/**
	* Light types
	*/
	ENUM() enum class NLS_RENDER_API ELightType : uint8_t
	{
		POINT,
		DIRECTIONAL,
		SPOT,
		AMBIENT_BOX,
		AMBIENT_SPHERE
	};
}
