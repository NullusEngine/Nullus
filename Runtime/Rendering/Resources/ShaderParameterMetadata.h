#pragma once

#include <string>
#include <vector>

#include "Rendering/RenderDef.h"
#include "Rendering/Resources/ShaderParameterStruct.h"

namespace NLS::Render::Resources
{
    struct NLS_RENDER_API ShaderRootParameterMetadata
    {
        std::string debugName;
        std::vector<ShaderParameterStruct> groups;

        [[nodiscard]] std::vector<ShaderParameterStruct> ToParameterStructs() const;
    };
}
