#pragma once

#include <memory>

#include "RenderDef.h"
#include "Rendering/RHI/Utils/RHIUIBridge.h"

namespace NLS::Render::RHI
{
    NLS_RENDER_API std::unique_ptr<RHIUIBridge> CreateMetalRHIUIBridge(
        const NativeRenderDeviceInfo& nativeDeviceInfo);
}
