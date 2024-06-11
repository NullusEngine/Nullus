#pragma once

#include <cstdint>
#include "RenderDef.h"
namespace NLS::Rendering::Data
{
	/**
	* Holds information about a given frame
	*/
	struct NLS_RENDER_API FrameInfo
	{
		uint64_t batchCount = 0;
		uint64_t instanceCount = 0;
		uint64_t polyCount = 0;
		uint64_t vertexCount = 0;
	};
}
