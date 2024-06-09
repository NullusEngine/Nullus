#pragma once

#include <cstdint>

namespace NLS::Rendering::Settings
{
	/**
	* OpenGL primitive mode enum wrapper
	*/
	enum class EPrimitiveMode : uint8_t
	{
		POINTS,
		LINES,
		LINE_LOOP,
		LINE_STRIP,
		TRIANGLES,
		TRIANGLE_STRIP,
		TRIANGLE_FAN,
		LINES_ADJACENCY,
		LINE_STRIP_ADJACENCY,
		TRIANGLES_ADJACENCY,
		TRIANGLE_STRIP_ADJACENCY,
		PATCHES
	};
}