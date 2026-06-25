#pragma once

#include <span>

#include "Rendering/Resources/ShaderReflection.h"

namespace NLS::Render::Resources
{
    NLS_RENDER_API bool TryMergeShaderReflection(
        ShaderReflection& destination,
        const ShaderReflection& source,
        std::string* diagnostic);

    NLS_RENDER_API bool TryMergePreferredShaderReflectionOrFallback(
        std::span<const ShaderReflection> preferredStages,
        std::span<const ShaderReflection> fallbackStages,
        ShaderReflection& destination,
        std::string* diagnostic);

    NLS_RENDER_API void MergeShaderReflection(
        ShaderReflection& destination,
        const ShaderReflection& source);
}
