#include "Rendering/Shaders/LightGridShaders.h"

namespace
{
    const NLS::Render::Resources::ShaderType& FindRequiredShaderType(std::string_view name)
    {
        return *NLS::Render::Resources::GetShaderTypeRegistry().FindByName(name);
    }
}

namespace NLS::Render::Engine::Shaders
{
    const Resources::ShaderType& LightGridResetCS::GetStaticShaderType() { return FindRequiredShaderType("LightGridResetCS"); }
    const Resources::ShaderType& LightGridInjectionCS::GetStaticShaderType() { return FindRequiredShaderType("LightGridInjectionCS"); }
    const Resources::ShaderType& LightGridCompactCS::GetStaticShaderType() { return FindRequiredShaderType("LightGridCompactCS"); }
}
