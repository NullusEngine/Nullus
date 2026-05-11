#pragma once

#include <map>
#include <memory>
#include <tuple>

#include "Rendering/RenderDef.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/ShaderPermutation.h"
#include "Rendering/Resources/ShaderType.h"

namespace NLS::Render::Resources
{
    class NLS_RENDER_API ShaderMap
    {
    public:
        void RegisterCompiledShader(
            const ShaderType* shaderType,
            ShaderPermutationId permutationId,
            Shader* shader);

        [[nodiscard]] Shader* FindCompiledShader(
            const ShaderType* shaderType,
            ShaderPermutationId permutationId = {}) const;

    private:
        using Key = std::tuple<const ShaderType*, uint32_t>;
        std::map<Key, Shader*> m_shaders;
    };

    template <typename TShaderClass>
    class ShaderMapRef
    {
    public:
        explicit ShaderMapRef(const ShaderMap& shaderMap, ShaderPermutationId permutationId = {})
            : m_shaderType(&TShaderClass::GetStaticShaderType())
            , m_shader(shaderMap.FindCompiledShader(m_shaderType, permutationId))
        {
        }

        [[nodiscard]] bool IsValid() const
        {
            return m_shaderType != nullptr;
        }

        [[nodiscard]] const ShaderType* GetShaderType() const
        {
            return m_shaderType;
        }

        [[nodiscard]] Shader* GetShader() const
        {
            return m_shader;
        }

    private:
        const ShaderType* m_shaderType = nullptr;
        Shader* m_shader = nullptr;
    };
}
