#pragma once

#include <cstdint>

namespace Rendering::Settings
{
    /**
    * Projection modes, mostly used for cameras
    */
    enum class EProjectionMode : uint8_t
    {
        ORTHOGRAPHIC,
        PERSPECTIVE
    };
}