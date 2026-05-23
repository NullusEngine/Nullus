#pragma once

#include <cstdint>

#include "RenderDef.h"

namespace NLS::Render::Context
{
    struct NLS_RENDER_API RenderFrameInput
    {
        uint64_t frameId = 0u;
        uint64_t sceneRevision = 0u;
        uint32_t renderWidth = 0u;
        uint32_t renderHeight = 0u;
        bool targetsSwapchain = true;
        bool hasExternalOutput = false;
        bool immutable = true;
        bool hasSceneInput = false;
        uint64_t sceneGameObjectCount = 0u;
        uint64_t visibleDrawCount = 0u;
        uint32_t externalOutputTextureCount = 0u;
    };
}
