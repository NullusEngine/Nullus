#pragma once

#include <cstdint>
#include <string>

#include "Rendering/RenderDef.h"

namespace NLS::Render::ShaderLab
{
    struct NLS_RENDER_API ShaderLabSourceLocation
    {
        std::string file;
        uint32_t line = 1;
        uint32_t column = 1;
        uint64_t byteOffset = 0;
    };
}
