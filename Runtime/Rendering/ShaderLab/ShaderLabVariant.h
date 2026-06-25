#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <set>
#include <string_view>
#include <vector>

#include "Guid.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "Rendering/ShaderLab/ShaderLabTypes.h"

namespace NLS::Render::ShaderLab
{
    struct ShaderLabPassRuntime;

    NLS_RENDER_API uint64_t HashShaderLabString(std::string_view value);
    NLS_RENDER_API void HashShaderLabCombine(uint64_t& seed, uint64_t value);
    NLS_RENDER_API void HashShaderLabCombineString(uint64_t& seed, std::string_view value);

    class NLS_RENDER_API ShaderLabKeywordSet
    {
    public:
        void Enable(std::string keyword);
        void Disable(std::string_view keyword);
        [[nodiscard]] bool Contains(std::string_view keyword) const;
        [[nodiscard]] std::vector<std::string> ToVector() const;
        [[nodiscard]] uint64_t Hash() const;

    private:
        std::set<std::string, std::less<>> m_keywords;
    };

    struct NLS_RENDER_API ShaderLabVariantKey
    {
        NLS::Guid shaderGuid = NLS::Guid::Empty();
        uint32_t subShaderIndex = 0;
        uint32_t passIndex = 0;
        NLS::Render::ShaderCompiler::ShaderStage stage =
            NLS::Render::ShaderCompiler::ShaderStage::Vertex;
        uint64_t keywordHash = 0;
        NLS::Render::RHI::NativeBackendType backend = NLS::Render::RHI::NativeBackendType::None;
        ShaderLabShaderModel shaderModel = ShaderLabShaderModel::SM6_6;

        [[nodiscard]] uint64_t Hash() const;
    };

    struct NLS_RENDER_API ShaderLabArtifactKey
    {
        NLS::Guid shaderGuid = NLS::Guid::Empty();
        uint32_t subShaderIndex = 0;
        uint32_t passIndex = 0;
        NLS::Render::ShaderCompiler::ShaderStage stage =
            NLS::Render::ShaderCompiler::ShaderStage::Vertex;
        std::string entryPoint;
        uint64_t hlslSourceHash = 0;
        uint64_t includeDependencyHash = 0;
        uint64_t keywordHash = 0;
        NLS::Render::RHI::NativeBackendType backend = NLS::Render::RHI::NativeBackendType::None;
        NLS::Render::ShaderCompiler::ShaderTargetPlatform target =
            NLS::Render::ShaderCompiler::ShaderTargetPlatform::Unknown;
        ShaderLabShaderModel shaderModel = ShaderLabShaderModel::SM6_6;
        std::string compilerFingerprint;
        uint64_t compileArgumentsHash = 0;

        [[nodiscard]] uint64_t Hash() const;
    };

    struct NLS_RENDER_API ShaderLabArtifactKeyBuildInput
    {
        NLS::Guid shaderGuid = NLS::Guid::Empty();
        uint32_t subShaderIndex = 0;
        uint32_t passIndex = 0;
        NLS::Render::ShaderCompiler::ShaderCompilationInput compileInput;
        uint64_t hlslSourceHash = 0;
        uint64_t includeDependencyHash = 0;
        uint64_t keywordHash = 0;
        NLS::Render::RHI::NativeBackendType backend = NLS::Render::RHI::NativeBackendType::None;
        ShaderLabShaderModel shaderModel = ShaderLabShaderModel::SM6_6;
        std::string compilerFingerprint;
        // Optional supplemental hash for compile flags not represented in ShaderCompilationInput.
        uint64_t compileArgumentsHash = 0;
    };

    struct NLS_RENDER_API ShaderLabCompileRequestBuildInput
    {
        NLS::Guid shaderGuid = NLS::Guid::Empty();
        std::shared_ptr<const ShaderLabPassRuntime> pass;
        NLS::Render::ShaderCompiler::ShaderStage stage =
            NLS::Render::ShaderCompiler::ShaderStage::Vertex;
        NLS::Render::RHI::NativeBackendType backend = NLS::Render::RHI::NativeBackendType::None;
        NLS::Render::ShaderCompiler::ShaderTargetPlatform target =
            NLS::Render::ShaderCompiler::ShaderTargetPlatform::Unknown;
        ShaderLabShaderModel shaderModel = ShaderLabShaderModel::SM6_6;
        std::string compileSourcePath;
        std::vector<std::string> includeDirectories;
        ShaderLabKeywordSet keywords;
        uint64_t hlslSourceHash = 0;
        uint64_t includeDependencyHash = 0;
        std::string compilerFingerprint;
        uint64_t compileArgumentsHash = 0;
    };

    struct NLS_RENDER_API ShaderLabCompileRequest
    {
        ShaderLabVariantKey variantKey;
        NLS::Render::ShaderCompiler::ShaderCompilationInput compileInput;
        std::string sourceText;
        ShaderLabArtifactKey artifactKey;
    };

    NLS_RENDER_API std::vector<NLS::Render::ShaderCompiler::ShaderMacroDefinition>
        BuildShaderLabKeywordMacros(const ShaderLabKeywordSet& keywords);
    NLS_RENDER_API NLS::Render::ShaderCompiler::ShaderCompilationInput BuildShaderLabCompileInput(
        const ShaderLabPassRuntime& pass,
        NLS::Render::ShaderCompiler::ShaderStage stage,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform target,
        ShaderLabShaderModel shaderModel,
        std::string assetPath,
        std::vector<std::string> includeDirectories,
        const ShaderLabKeywordSet& keywords);
    NLS_RENDER_API ShaderLabCompileRequest BuildShaderLabCompileRequest(const ShaderLabCompileRequestBuildInput& input);
    NLS_RENDER_API ShaderLabArtifactKey BuildShaderLabArtifactKey(const ShaderLabArtifactKeyBuildInput& input);
}
