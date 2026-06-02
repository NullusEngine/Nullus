#pragma once

#include <cstdint>

#include "RenderDef.h"

namespace NLS::Render::Data
{
	struct NLS_RENDER_API DrawCallOptimizationStats
	{
		uint64_t rawVisibleObjectCount = 0u;
		uint64_t submittedSceneDrawCount = 0u;
		uint64_t dynamicInstanceGroupCount = 0u;
		uint64_t largestInstanceGroupSize = 0u;
		uint64_t cachedCommandRebuildCount = 0u;
		uint64_t objectDataOverflowDroppedObjectCount = 0u;
	};
}
