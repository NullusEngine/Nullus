#pragma once
#include "RenderDef.h"
namespace NLS::Render::Settings
{
	/**
	* Backend-agnostic pixel data format enumeration
	*/
	enum class NLS_RENDER_API EPixelDataFormat : uint8_t
	{
		COLOR_INDEX = 0,
		STENCIL_INDEX,
		DEPTH_COMPONENT,
		RED,
		GREEN,
		BLUE,
		ALPHA,
		RGB,
		BGR,
		RGBA,
		BGRA,
		LUMINANCE,
		LUMINANCE_ALPHA,
	};
}
