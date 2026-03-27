#pragma once
#include "RenderDef.h"
namespace NLS::Render::Settings
{
	/**
	* Defines some access hints that buffers can use
	*/
	enum class NLS_RENDER_API EAccessSpecifier : uint8_t
	{
		STREAM_DRAW = 0,
		STREAM_READ,
		STREAM_COPY,
		DYNAMIC_DRAW,
		DYNAMIC_READ,
		DYNAMIC_COPY,
		STATIC_DRAW,
		STATIC_READ,
		STATIC_COPY
	};
}
