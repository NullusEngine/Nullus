#pragma once
#include "RenderDef.h"
namespace NLS::Rendering::Settings
{
	/**
	* OpenGL texture filtering mode enum wrapper
	*/
	// TODO: Dissociate the values from OpenGL values, and convert them in the driver.cpp
	// Would require the driver.cpp to wrap all GL functions
	enum class NLS_RENDER_API ETextureFilteringMode
	{
		NEAREST					= 0x2600,
		LINEAR					= 0x2601,
		NEAREST_MIPMAP_NEAREST	= 0x2700,
		LINEAR_MIPMAP_LINEAR	= 0x2703,
		LINEAR_MIPMAP_NEAREST	= 0x2701,
		NEAREST_MIPMAP_LINEAR	= 0x2702
	};
}