#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "Rendering/RenderDef.h"
#include "Rendering/Resources/ShaderParameterMetadata.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/Resources/ShaderPermutation.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"

namespace NLS::Render::Resources
{
    enum class NLS_RENDER_API ShaderTypeKind : uint8_t
    {
        Global,
        Material
    };

    class NLS_RENDER_API ShaderType
    {
    public:
        using ShouldCompilePredicate = std::function<bool(const ShaderPermutationParameters&)>;

        ShaderType(
            std::string name,
            std::string sourcePath,
            std::string entryPoint,
            ShaderCompiler::ShaderStage stage,
            ShaderTypeKind kind,
            ShaderRootParameterMetadata rootParameterMetadata,
            ShouldCompilePredicate shouldCompile = {});

        [[nodiscard]] std::string_view GetName() const;
        [[nodiscard]] std::string_view GetSourcePath() const;
        [[nodiscard]] std::string_view GetEntryPoint() const;
        [[nodiscard]] ShaderCompiler::ShaderStage GetStage() const;
        [[nodiscard]] ShaderTypeKind GetKind() const;
        [[nodiscard]] const std::vector<ShaderParameterStruct>& GetRootParameterStructs() const;
        [[nodiscard]] const ShaderRootParameterMetadata* GetRootParameterMetadata() const;
        [[nodiscard]] bool ShouldCompilePermutation(const ShaderPermutationParameters& parameters) const;

    private:
        std::string m_name;
        std::string m_sourcePath;
        std::string m_entryPoint;
        ShaderCompiler::ShaderStage m_stage = ShaderCompiler::ShaderStage::Vertex;
        ShaderTypeKind m_kind = ShaderTypeKind::Global;
        ShaderRootParameterMetadata m_rootParameterMetadata;
        ShouldCompilePredicate m_shouldCompile;
    };

    class NLS_RENDER_API ShaderTypeRegistry
    {
    public:
        void Register(ShaderType shaderType);

        [[nodiscard]] const ShaderType* FindByName(std::string_view name) const;
        [[nodiscard]] std::vector<const ShaderType*> FindBySourcePath(std::string_view sourcePath) const;
        [[nodiscard]] const std::vector<ShaderType>& GetShaderTypes() const;

    private:
        std::vector<ShaderType> m_shaderTypes;
    };

    NLS_RENDER_API const ShaderTypeRegistry& GetShaderTypeRegistry();
}
