#pragma once

#include "Rendering/RenderDef.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/Resources/ShaderType.h"

namespace NLS::Render::Resources
{
    struct NLS_RENDER_API ShaderParameterBindResult
    {
        bool success = false;
        uint32_t boundSetCount = 0u;
    };

    class NLS_RENDER_API ShaderParameterBinder
    {
    public:
        static ShaderParameterBindResult SetShaderParameters(
            RHI::RHICommandBuffer& commandBuffer,
            const ShaderType& shaderType,
            const std::vector<ShaderParameterStruct>& parameters);
    };
}
