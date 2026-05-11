#pragma once

#include "EngineDef.h"
#include "Rendering/Resources/ShaderType.h"

namespace NLS::Render::Engine::Shaders
{
    class NLS_ENGINE_API StandardVS { public: static const Resources::ShaderType& GetStaticShaderType(); };
    class NLS_ENGINE_API StandardPS { public: static const Resources::ShaderType& GetStaticShaderType(); };
    class NLS_ENGINE_API LambertVS { public: static const Resources::ShaderType& GetStaticShaderType(); };
    class NLS_ENGINE_API LambertPS { public: static const Resources::ShaderType& GetStaticShaderType(); };
    class NLS_ENGINE_API StandardPBRVS { public: static const Resources::ShaderType& GetStaticShaderType(); };
    class NLS_ENGINE_API StandardPBRPS { public: static const Resources::ShaderType& GetStaticShaderType(); };
    class NLS_ENGINE_API DeferredGBufferVS { public: static const Resources::ShaderType& GetStaticShaderType(); };
    class NLS_ENGINE_API DeferredGBufferPS { public: static const Resources::ShaderType& GetStaticShaderType(); };
}
