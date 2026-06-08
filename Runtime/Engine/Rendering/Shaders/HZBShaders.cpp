#include "Rendering/Shaders/HZBShaders.h"

#include <stdexcept>
#include <string>

namespace NLS::Render::Engine::Shaders
{
    const Resources::ShaderType& ResolveRequiredHZBShaderType(
        const Resources::ShaderTypeRegistry& registry,
        const std::string_view name)
    {
        const auto* shaderType = registry.FindByName(name);
        if (shaderType == nullptr)
            throw std::runtime_error("Required HZB shader type is not registered: " + std::string(name));
        return *shaderType;
    }

    const Resources::ShaderType& HZBBuildCS::GetStaticShaderType()
    {
        return ResolveRequiredHZBShaderType(Resources::GetShaderTypeRegistry(), "HZBBuildCS");
    }

    const Resources::ShaderType& HZBOcclusionCS::GetStaticShaderType()
    {
        return ResolveRequiredHZBShaderType(Resources::GetShaderTypeRegistry(), "HZBOcclusionCS");
    }
}
