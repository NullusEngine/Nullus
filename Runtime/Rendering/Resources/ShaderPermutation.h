#pragma once

#include <cstdint>

#include "Rendering/RenderDef.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"

namespace NLS::Render::Resources
{
    struct NLS_RENDER_API ShaderPermutationId
    {
        uint32_t value = 0u;

        [[nodiscard]] constexpr bool operator==(const ShaderPermutationId& other) const
        {
            return value == other.value;
        }

        [[nodiscard]] constexpr bool operator!=(const ShaderPermutationId& other) const
        {
            return !(*this == other);
        }
    };

    struct NLS_RENDER_API ShaderPermutationParameters
    {
        ShaderCompiler::ShaderTargetPlatform targetPlatform = ShaderCompiler::ShaderTargetPlatform::Unknown;
        ShaderPermutationId permutationId;
    };
}
