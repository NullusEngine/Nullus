#include "Rendering/Resources/ShaderParameterBinder.h"

namespace NLS::Render::Resources
{
    ShaderParameterBindResult ShaderParameterBinder::SetShaderParameters(
        RHI::RHICommandBuffer&,
        const ShaderType& shaderType,
        const std::vector<ShaderParameterStruct>& parameters)
    {
        ShaderParameterBindResult result;
        result.success = shaderType.GetRootParameterMetadata() != nullptr;
        result.boundSetCount = static_cast<uint32_t>(parameters.size());
        return result;
    }
}
