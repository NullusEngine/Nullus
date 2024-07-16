#pragma once

#include <cstdint>
#include "RenderDef.h"
namespace NLS::Render::Settings
{
	/**
	* Enumeration of cullable face
	*/
	enum class NLS_RENDER_API ECullFace : uint8_t
	{
		FRONT,
		BACK,
		FRONT_AND_BACK
	};
}