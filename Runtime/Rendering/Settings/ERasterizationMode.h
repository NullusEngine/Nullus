#pragma once

#include <cstdint>
#include "RenderDef.h"
namespace NLS::Render::Settings
{
	/**
	* Rasterization mode enum wrapper
	*/
	enum class NLS_RENDER_API ERasterizationMode : uint8_t
	{
		POINT,	// Polygon vertices that are marked as the start of a boundary edge are drawn as points.
		LINE,	// Boundary edges of the polygon are drawn as line segments.
		FILL	// The interior of the polygon is filled.
	};
}
