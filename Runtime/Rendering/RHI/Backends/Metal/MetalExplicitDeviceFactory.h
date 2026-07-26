#pragma once

#include <memory>

#include "RenderDef.h"

namespace NLS::Render::RHI
{
    class RHIDevice;
}

namespace NLS::Render::Backend
{
    // First Metal milestone: device, drawable surface, UI textures, and direct ImGui presentation.
    // Generic scene command encoding and pipeline translation are intentionally not exposed as supported.
    NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateMetalRhiDevice(bool debugMode = false);
}
