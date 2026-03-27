#pragma once
#include "RenderDef.h"
namespace NLS::Render::Settings
{
	/**
	* Generic vertex attribute element type
	*/
	enum class NLS_RENDER_API EDataType
	{
		BYTE = 0,
		UNISGNED_BYTE,
		SHORT,
		UNSIGNED_SHORT,
		INT,
		UNSIGNED_INT,
		FLOAT,
		DOUBLE
	};
}
