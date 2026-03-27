#pragma once
#include "RenderDef.h"
namespace NLS::Render::Settings
{
	/**
	* Texture filtering mode
	*/
	enum class NLS_RENDER_API ETextureFilteringMode
	{
		NEAREST = 0,
		LINEAR,
		NEAREST_MIPMAP_NEAREST,
		LINEAR_MIPMAP_LINEAR,
		LINEAR_MIPMAP_NEAREST,
		NEAREST_MIPMAP_LINEAR
	};
}
