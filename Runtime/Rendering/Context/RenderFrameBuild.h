#pragma once

#include <cstdint>

#include "RenderDef.h"

namespace NLS::Render::Context
{
    struct NLS_RENDER_API RenderFrameBuild
    {
        uint64_t frameId = 0u;
        bool renderThreadOwned = true;
        bool targetsSwapchain = true;
        bool hasVisibleDraws = false;
        bool frameDataReady = false;
        bool objectDataReady = false;
        bool lightingDataReady = false;
        uint64_t visibleDrawCount = 0u;
        uint64_t passPlanCount = 0u;
        uint64_t drawCommandCount = 0u;
        bool containsParallelCommandWorkUnits = false;
    };
}
