#include "Rendering/ShaderLab/ShaderLabVariant.h"

#include <algorithm>

#include "Rendering/ShaderLab/ShaderLabAsset.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"

namespace NLS::Render::ShaderLab
{
    namespace
    {
        void HashGuid(uint64_t& seed, const NLS::Guid& guid)
        {
            for (const uint8_t byte : guid.GetBytes())
                HashShaderLabCombine(seed, byte);
        }

        const std::string& GetEntryPointForStage(
            const ShaderLabPassRuntime& pass,
            const NLS::Render::ShaderCompiler::ShaderStage stage)
        {
            switch (stage)
            {
            case NLS::Render::ShaderCompiler::ShaderStage::Vertex: return pass.vertexEntry;
            case NLS::Render::ShaderCompiler::ShaderStage::Pixel: return pass.fragmentEntry;
            case NLS::Render::ShaderCompiler::ShaderStage::Compute: return pass.computeEntry;
            default: return pass.vertexEntry;
            }
        }

        const char* GetTargetProfilePrefix(const NLS::Render::ShaderCompiler::ShaderStage stage)
        {
            switch (stage)
            {
            case NLS::Render::ShaderCompiler::ShaderStage::Vertex: return "vs";
            case NLS::Render::ShaderCompiler::ShaderStage::Pixel: return "ps";
            case NLS::Render::ShaderCompiler::ShaderStage::Compute: return "cs";
            default: return "unknown";
            }
        }

        const char* GetShaderModelSuffix(const ShaderLabShaderModel shaderModel)
        {
            switch (shaderModel)
            {
            case ShaderLabShaderModel::SM6_0: return "6_0";
            case ShaderLabShaderModel::SM6_6: return "6_6";
            default: return "6_6";
            }
        }

        uint64_t HashCompileOptions(const NLS::Render::ShaderCompiler::ShaderCompileOptions& options)
        {
            uint64_t hash = 0;
            HashShaderLabCombineString(hash, options.entryPoint);
            HashShaderLabCombineString(hash, options.targetProfile);
            HashShaderLabCombine(hash, static_cast<uint64_t>(options.targetPlatform));
            HashShaderLabCombine(hash, static_cast<uint64_t>(options.sourceLanguage));
            HashShaderLabCombine(hash, options.enableDebugInfo ? 1u : 0u);
            HashShaderLabCombine(hash, options.treatWarningsAsErrors ? 1u : 0u);
            for (const auto& includeDirectory : options.includeDirectories)
                HashShaderLabCombineString(hash, includeDirectory);
            for (const auto& macro : options.macros)
            {
                HashShaderLabCombineString(hash, macro.name);
                HashShaderLabCombineString(hash, macro.value);
            }
            return hash;
        }
    }

    uint64_t HashShaderLabString(const std::string_view value)
    {
        uint64_t hash = 14695981039346656037ull;
        for (const unsigned char c : value)
        {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    void HashShaderLabCombine(uint64_t& seed, const uint64_t value)
    {
        seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    }

    void HashShaderLabCombineString(uint64_t& seed, const std::string_view value)
    {
        HashShaderLabCombine(seed, HashShaderLabString(value));
    }

    void ShaderLabKeywordSet::Enable(std::string keyword)
    {
        if (!keyword.empty())
            m_keywords.insert(std::move(keyword));
    }

    void ShaderLabKeywordSet::Disable(const std::string_view keyword)
    {
        const auto found = m_keywords.find(keyword);
        if (found != m_keywords.end())
            m_keywords.erase(found);
    }

    bool ShaderLabKeywordSet::Contains(const std::string_view keyword) const
    {
        return m_keywords.find(keyword) != m_keywords.end();
    }

    std::vector<std::string> ShaderLabKeywordSet::ToVector() const
    {
        return { m_keywords.begin(), m_keywords.end() };
    }

    uint64_t ShaderLabKeywordSet::Hash() const
    {
        uint64_t hash = 0;
        for (const auto& keyword : m_keywords)
        {
            if (keyword.empty() || keyword == "_")
                continue;
            HashShaderLabCombineString(hash, keyword);
        }
        return hash;
    }

    uint64_t ShaderLabVariantKey::Hash() const
    {
        uint64_t hash = 0;
        HashGuid(hash, shaderGuid);
        HashShaderLabCombine(hash, subShaderIndex);
        HashShaderLabCombine(hash, passIndex);
        HashShaderLabCombine(hash, static_cast<uint64_t>(stage));
        HashShaderLabCombine(hash, keywordHash);
        HashShaderLabCombine(hash, static_cast<uint64_t>(backend));
        HashShaderLabCombine(hash, static_cast<uint64_t>(shaderModel));
        return hash;
    }

    uint64_t ShaderLabArtifactKey::Hash() const
    {
        uint64_t hash = 0;
        HashGuid(hash, shaderGuid);
        HashShaderLabCombine(hash, subShaderIndex);
        HashShaderLabCombine(hash, passIndex);
        HashShaderLabCombine(hash, static_cast<uint64_t>(stage));
        HashShaderLabCombineString(hash, entryPoint);
        HashShaderLabCombine(hash, hlslSourceHash);
        HashShaderLabCombine(hash, includeDependencyHash);
        HashShaderLabCombine(hash, keywordHash);
        HashShaderLabCombine(hash, static_cast<uint64_t>(backend));
        HashShaderLabCombine(hash, static_cast<uint64_t>(target));
        HashShaderLabCombine(hash, static_cast<uint64_t>(shaderModel));
        HashShaderLabCombineString(hash, compilerFingerprint);
        HashShaderLabCombine(hash, compileArgumentsHash);
        return hash;
    }

    std::vector<NLS::Render::ShaderCompiler::ShaderMacroDefinition>
        BuildShaderLabKeywordMacros(const ShaderLabKeywordSet& keywords)
    {
        std::vector<NLS::Render::ShaderCompiler::ShaderMacroDefinition> macros;
        for (const auto& keyword : keywords.ToVector())
        {
            if (keyword.empty() || keyword == "_")
                continue;

            macros.push_back({ keyword, "1" });
        }
        return macros;
    }

    NLS::Render::ShaderCompiler::ShaderCompilationInput BuildShaderLabCompileInput(
        const ShaderLabPassRuntime& pass,
        const NLS::Render::ShaderCompiler::ShaderStage stage,
        const NLS::Render::ShaderCompiler::ShaderTargetPlatform target,
        const ShaderLabShaderModel shaderModel,
        std::string compileSourcePath,
        std::vector<std::string> includeDirectories,
        const ShaderLabKeywordSet& keywords)
    {
        NLS::Render::ShaderCompiler::ShaderCompilationInput input;
        input.assetPath = std::move(compileSourcePath);
        input.stage = stage;
        input.options.sourceLanguage = NLS::Render::ShaderCompiler::ShaderSourceLanguage::HLSL;
        input.options.targetPlatform = target;
        input.options.entryPoint = GetEntryPointForStage(pass, stage);
        input.options.targetProfile =
            std::string(GetTargetProfilePrefix(stage)) + "_" + GetShaderModelSuffix(shaderModel);
        input.options.includeDirectories = std::move(includeDirectories);
        input.options.macros = BuildShaderLabKeywordMacros(keywords);
        return input;
    }

    ShaderLabCompileRequest BuildShaderLabCompileRequest(const ShaderLabCompileRequestBuildInput& input)
    {
        ShaderLabCompileRequest request;
        if (input.pass == nullptr)
            return request;

        const auto& pass = *input.pass;
        request.variantKey.shaderGuid = input.shaderGuid;
        request.variantKey.subShaderIndex = pass.subShaderIndex;
        request.variantKey.passIndex = pass.passIndex;
        request.variantKey.stage = input.stage;
        request.variantKey.keywordHash = input.keywords.Hash();
        request.variantKey.backend = input.backend;
        request.variantKey.shaderModel = input.shaderModel;

        request.compileInput = BuildShaderLabCompileInput(
            pass,
            input.stage,
            input.target,
            input.shaderModel,
            input.compileSourcePath,
            input.includeDirectories,
            input.keywords);

        request.sourceText = BuildShaderLabHlslForCompile(pass);

        ShaderLabArtifactKeyBuildInput artifactInput;
        artifactInput.shaderGuid = input.shaderGuid;
        artifactInput.subShaderIndex = pass.subShaderIndex;
        artifactInput.passIndex = pass.passIndex;
        artifactInput.compileInput = request.compileInput;
        artifactInput.hlslSourceHash = input.hlslSourceHash;
        artifactInput.includeDependencyHash = input.includeDependencyHash;
        artifactInput.keywordHash = input.keywords.Hash();
        artifactInput.backend = input.backend;
        artifactInput.shaderModel = input.shaderModel;
        artifactInput.compilerFingerprint = input.compilerFingerprint;
        artifactInput.compileArgumentsHash = input.compileArgumentsHash;
        request.artifactKey = BuildShaderLabArtifactKey(artifactInput);
        return request;
    }

    ShaderLabArtifactKey BuildShaderLabArtifactKey(const ShaderLabArtifactKeyBuildInput& input)
    {
        ShaderLabArtifactKey key;
        key.shaderGuid = input.shaderGuid;
        key.subShaderIndex = input.subShaderIndex;
        key.passIndex = input.passIndex;
        key.stage = input.compileInput.stage;
        key.entryPoint = input.compileInput.options.entryPoint;
        key.hlslSourceHash = input.hlslSourceHash;
        key.includeDependencyHash = input.includeDependencyHash;
        key.keywordHash = input.keywordHash;
        key.backend = input.backend;
        key.target = input.compileInput.options.targetPlatform;
        key.shaderModel = input.shaderModel;
        key.compilerFingerprint = input.compilerFingerprint;
        key.compileArgumentsHash =
            HashCompileOptions(input.compileInput.options) ^ input.compileArgumentsHash;
        return key;
    }
}
