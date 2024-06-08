#pragma once

#include <cstdint>

namespace Rendering::Data
{
	/**
	* Holds information about a given frame
	*/
	struct FrameInfo
	{
		uint64_t batchCount = 0;
		uint64_t instanceCount = 0;
		uint64_t polyCount = 0;
		uint64_t vertexCount = 0;
	};
}
