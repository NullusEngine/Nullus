#pragma once

#include <cstdint>
#include "RenderDef.h"
#include "Reflection/Macros.h"
namespace NLS::Render::Settings
{
    /**
    * Projection modes, mostly used for cameras
    */
    ENUM(NLS_RENDER_API EProjectionMode) : uint8_t
    {
        ORTHOGRAPHIC,
        PERSPECTIVE
    };
}
