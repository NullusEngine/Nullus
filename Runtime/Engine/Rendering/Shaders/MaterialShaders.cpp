#include "Rendering/Shaders/MaterialShaders.h"

namespace
{
    const NLS::Render::Resources::ShaderType& FindRequiredShaderType(std::string_view name)
    {
        const auto* shaderType = NLS::Render::Resources::GetShaderTypeRegistry().FindByName(name);
        return *shaderType;
    }
}

namespace NLS::Render::Engine::Shaders
{
    const Resources::ShaderType& StandardVS::GetStaticShaderType() { return FindRequiredShaderType("StandardVS"); }
    const Resources::ShaderType& StandardPS::GetStaticShaderType() { return FindRequiredShaderType("StandardPS"); }
    const Resources::ShaderType& LambertVS::GetStaticShaderType() { return FindRequiredShaderType("LambertVS"); }
    const Resources::ShaderType& LambertPS::GetStaticShaderType() { return FindRequiredShaderType("LambertPS"); }
    const Resources::ShaderType& StandardPBRVS::GetStaticShaderType() { return FindRequiredShaderType("StandardPBRVS"); }
    const Resources::ShaderType& StandardPBRPS::GetStaticShaderType() { return FindRequiredShaderType("StandardPBRPS"); }
    const Resources::ShaderType& DeferredGBufferVS::GetStaticShaderType() { return FindRequiredShaderType("DeferredGBufferVS"); }
    const Resources::ShaderType& DeferredGBufferPS::GetStaticShaderType() { return FindRequiredShaderType("DeferredGBufferPS"); }
}
