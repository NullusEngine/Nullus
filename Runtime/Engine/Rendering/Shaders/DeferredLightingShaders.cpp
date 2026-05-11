#include "Rendering/Shaders/DeferredLightingShaders.h"

namespace NLS::Render::Engine::Shaders
{
    const Resources::ShaderType& DeferredLightingPS::GetStaticShaderType()
    {
        return *Resources::GetShaderTypeRegistry().FindByName("DeferredLightingPS");
    }
}
