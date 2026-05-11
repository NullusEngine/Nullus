#include "Rendering/Resources/ShaderMap.h"

namespace NLS::Render::Resources
{
    void ShaderMap::RegisterCompiledShader(
        const ShaderType* shaderType,
        ShaderPermutationId permutationId,
        Shader* shader)
    {
        if (shaderType == nullptr)
            return;

        m_shaders[{ shaderType, permutationId.value }] = shader;
    }

    Shader* ShaderMap::FindCompiledShader(
        const ShaderType* shaderType,
        ShaderPermutationId permutationId) const
    {
        const auto found = m_shaders.find({ shaderType, permutationId.value });
        return found != m_shaders.end() ? found->second : nullptr;
    }
}
