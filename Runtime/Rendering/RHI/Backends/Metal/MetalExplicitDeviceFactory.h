#pragma once

#include <memory>

#include "RenderDef.h"

namespace NLS::Render::RHI
{
    class RHIDevice;
}

namespace NLS::Render::Backend
{
    // Creates the explicit Metal backend used by scene, compute, UI-overlay, and presentation paths.
    NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateMetalRhiDevice(bool debugMode = false);
}
