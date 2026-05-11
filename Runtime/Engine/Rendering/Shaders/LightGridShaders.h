#pragma once

#include "EngineDef.h"
#include "Rendering/Resources/ShaderType.h"

namespace NLS::Render::Engine::Shaders
{
    class NLS_ENGINE_API LightGridResetCS { public: static const Resources::ShaderType& GetStaticShaderType(); };
    class NLS_ENGINE_API LightGridInjectionCS { public: static const Resources::ShaderType& GetStaticShaderType(); };
    class NLS_ENGINE_API LightGridCompactCS { public: static const Resources::ShaderType& GetStaticShaderType(); };
}
