#pragma once

#include <string_view>

#include "EngineDef.h"
#include "Rendering/Resources/ShaderType.h"

namespace NLS::Render::Engine::Shaders
{
    NLS_ENGINE_API const Resources::ShaderType& ResolveRequiredHZBShaderType(
        const Resources::ShaderTypeRegistry& registry,
        std::string_view name);

    class NLS_ENGINE_API HZBBuildCS { public: static const Resources::ShaderType& GetStaticShaderType(); };
    class NLS_ENGINE_API HZBOcclusionCS { public: static const Resources::ShaderType& GetStaticShaderType(); };
}
