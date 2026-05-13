#include "Rendering/Resources/ShaderType.h"

#include <algorithm>

#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/RHI/BindingPointMap.h"

namespace
{
    constexpr uint32_t kForwardLightDataByteSize = 352u;

    using NLS::Render::Resources::ShaderParameterGroupKind;
    using NLS::Render::Resources::ShaderRootParameterMetadata;
    using NLS::Render::Resources::ShaderParameterStruct;
    using NLS::Render::Resources::ShaderParameterStructBuilder;
    using NLS::Render::Resources::ShaderType;
    using NLS::Render::Resources::ShaderTypeKind;
    using NLS::Render::Resources::ShaderTypeRegistry;

    std::string NormalizeShaderPath(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }

    bool PathMatchesShaderTypeSource(std::string_view queryPath, std::string_view shaderTypeSourcePath)
    {
        const auto normalizedQuery = NormalizeShaderPath(std::string(queryPath));
        const auto normalizedSource = NormalizeShaderPath(std::string(shaderTypeSourcePath));
        if (normalizedQuery == normalizedSource)
            return true;

        if (normalizedQuery.size() <= normalizedSource.size())
            return false;

        const auto suffixOffset = normalizedQuery.size() - normalizedSource.size();
        return normalizedQuery.compare(suffixOffset, normalizedSource.size(), normalizedSource) == 0 &&
            normalizedQuery[suffixOffset - 1u] == '/';
    }

    ShaderParameterStruct BuildFrameParameters(std::string debugName)
    {
        return ShaderParameterStructBuilder(std::move(debugName))
            .SetGroup(ShaderParameterGroupKind::Frame)
            .AddUniformBuffer("FrameConstants", 0u, 144u, NLS::Render::RHI::ShaderStageMask::AllGraphics)
            .Build();
    }

    ShaderParameterStruct BuildObjectParameters(std::string debugName)
    {
        return ShaderParameterStructBuilder(std::move(debugName))
            .SetGroup(ShaderParameterGroupKind::Object)
            .AddUniformBuffer("ObjectConstants", 0u, 64u, NLS::Render::RHI::ShaderStageMask::Vertex)
            .Build();
    }

    ShaderParameterStruct BuildEmptyObjectParameters(std::string debugName)
    {
        return ShaderParameterStructBuilder(std::move(debugName))
            .SetGroup(ShaderParameterGroupKind::Object)
            .Build();
    }

    ShaderParameterStruct BuildLightGridGraphicsPassParameters(std::string debugName)
    {
        return ShaderParameterStructBuilder(std::move(debugName))
            .SetGroup(ShaderParameterGroupKind::Pass)
            .AddUniformBuffer("ForwardLightData", 0u, kForwardLightDataByteSize, NLS::Render::RHI::ShaderStageMask::Fragment)
            .AddStructuredBuffer("u_ForwardLocalLightBuffer", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
            .AddStructuredBuffer("u_NumCulledLightsGrid", 1u, NLS::Render::RHI::ShaderStageMask::Fragment)
            .AddStructuredBuffer("u_CulledLightDataGrid", 2u, NLS::Render::RHI::ShaderStageMask::Fragment)
            .Build();
    }

    ShaderRootParameterMetadata MakeRootMetadata(std::string debugName, std::vector<ShaderParameterStruct> groups)
    {
        ShaderRootParameterMetadata metadata;
        metadata.debugName = std::move(debugName);
        metadata.groups = std::move(groups);
        return metadata;
    }

    ShaderRootParameterMetadata BuildStandardParameters(std::string debugName)
    {
        return MakeRootMetadata(std::move(debugName), {
            BuildFrameParameters("StandardFrameParameters"),
            ShaderParameterStructBuilder("StandardMaterialParameters")
                .SetGroup(ShaderParameterGroupKind::Material)
                .AddUniformBuffer("MaterialConstants", 0u, 64u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_DiffuseMap", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_SpecularMap", 1u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_NormalMap", 2u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_HeightMap", 3u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_MaskMap", 4u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddSampler("u_LinearWrapSampler", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .Build(),
            BuildObjectParameters("StandardObjectParameters"),
            BuildLightGridGraphicsPassParameters("StandardPassParameters")
        });
    }

    ShaderRootParameterMetadata BuildLambertParameters(std::string debugName)
    {
        return MakeRootMetadata(std::move(debugName), {
            BuildFrameParameters("LambertFrameParameters"),
            ShaderParameterStructBuilder("LambertMaterialParameters")
                .SetGroup(ShaderParameterGroupKind::Material)
                .AddUniformBuffer("MaterialConstants", 0u, 32u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_DiffuseMap", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddSampler("u_LinearWrapSampler", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .Build(),
            BuildObjectParameters("LambertObjectParameters"),
            BuildLightGridGraphicsPassParameters("LambertPassParameters")
        });
    }

    ShaderRootParameterMetadata BuildStandardPBRParameters(std::string debugName)
    {
        return MakeRootMetadata(std::move(debugName), {
            BuildFrameParameters("StandardPBRFrameParameters"),
            ShaderParameterStructBuilder("StandardPBRMaterialParameters")
                .SetGroup(ShaderParameterGroupKind::Material)
                .AddUniformBuffer("MaterialConstants", 0u, 32u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_AlbedoMap", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_MetallicMap", 1u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_RoughnessMap", 2u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_AmbientOcclusionMap", 3u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_NormalMap", 4u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddSampler("u_LinearWrapSampler", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .Build(),
            BuildObjectParameters("StandardPBRObjectParameters"),
            BuildLightGridGraphicsPassParameters("StandardPBRPassParameters")
        });
    }

    ShaderRootParameterMetadata BuildDeferredLightingParameters(std::string debugName)
    {
        return MakeRootMetadata(std::move(debugName), {
            ShaderParameterStructBuilder("DeferredLightingFrameParameters")
                .SetGroup(ShaderParameterGroupKind::Frame)
                .Build(),
            ShaderParameterStructBuilder("DeferredLightingMaterialParameters")
                .SetGroup(ShaderParameterGroupKind::Material)
                .AddUniformBuffer("MaterialConstants", 0u, 96u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_GBufferAlbedo", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_GBufferNormal", 1u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_GBufferMaterial", 2u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_GBufferDepth", 3u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_SkyboxCube", 4u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddSampler("u_LinearWrapSampler", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .Build(),
            BuildEmptyObjectParameters("DeferredLightingObjectParameters"),
            BuildLightGridGraphicsPassParameters("DeferredLightingPassParameters")
        });
    }

    ShaderRootParameterMetadata BuildDeferredGBufferParameters(std::string debugName)
    {
        return MakeRootMetadata(std::move(debugName), {
            BuildFrameParameters("DeferredGBufferFrameParameters"),
            ShaderParameterStructBuilder("DeferredGBufferMaterialParameters")
                .SetGroup(ShaderParameterGroupKind::Material)
                .AddUniformBuffer("MaterialConstants", 0u, 32u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_AlbedoMap", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_MetallicMap", 1u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_RoughnessMap", 2u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_AmbientOcclusionMap", 3u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddTexture("u_NormalMap", 4u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .AddSampler("u_LinearWrapSampler", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
                .Build(),
            BuildObjectParameters("DeferredGBufferObjectParameters")
        });
    }

    ShaderRootParameterMetadata BuildLightGridResetParameters()
    {
        return MakeRootMetadata("LightGridResetCSRootParameters", {
            ShaderParameterStructBuilder("LightGridResetParameters")
                .SetGroup(ShaderParameterGroupKind::Pass)
                .AddUniformBuffer("Forward", 0u, 64u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_LightGridStartOffsetGrid", 1u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_LightGridCulledLightLinks", 2u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_LightGridLinkCounter", 3u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_LightGridCompactCounter", 4u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_NumCulledLightsGrid", 5u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_CulledLightDataGrid", 6u, NLS::Render::RHI::ShaderStageMask::Compute)
                .Build()
        });
    }

    ShaderRootParameterMetadata BuildLightGridInjectionParameters()
    {
        return MakeRootMetadata("LightGridInjectionCSRootParameters", {
            ShaderParameterStructBuilder("LightGridInjectionParameters")
                .SetGroup(ShaderParameterGroupKind::Pass)
                .AddUniformBuffer("Forward", 0u, 64u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStructuredBuffer("u_ForwardLocalLightBuffer", 0u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_LightGridStartOffsetGrid", 1u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_LightGridCulledLightLinks", 2u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_LightGridLinkCounter", 3u, NLS::Render::RHI::ShaderStageMask::Compute)
                .Build()
        });
    }

    ShaderRootParameterMetadata BuildLightGridCompactParameters()
    {
        return MakeRootMetadata("LightGridCompactCSRootParameters", {
            ShaderParameterStructBuilder("LightGridCompactParameters")
                .SetGroup(ShaderParameterGroupKind::Pass)
                .AddUniformBuffer("Forward", 0u, 64u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStructuredBuffer("u_LightGridStartOffsetGrid", 1u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStructuredBuffer("u_LightGridCulledLightLinks", 2u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_LightGridCompactCounter", 3u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_NumCulledLightsGrid", 4u, NLS::Render::RHI::ShaderStageMask::Compute)
                .AddStorageBuffer("u_CulledLightDataGrid", 5u, NLS::Render::RHI::ShaderStageMask::Compute)
                .Build()
        });
    }

    ShaderTypeRegistry BuildEngineShaderTypeRegistry()
    {
        ShaderTypeRegistry registry;
        registry.Register({
            "StandardVS",
            "App/Assets/Engine/Shaders/Standard.hlsl",
            "VSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Vertex,
            ShaderTypeKind::Material,
            BuildStandardParameters("StandardVSRootParameters")
        });
        registry.Register({
            "StandardPS",
            "App/Assets/Engine/Shaders/Standard.hlsl",
            "PSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            ShaderTypeKind::Material,
            BuildStandardParameters("StandardPSRootParameters")
        });
        registry.Register({
            "LambertVS",
            "App/Assets/Engine/Shaders/Lambert.hlsl",
            "VSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Vertex,
            ShaderTypeKind::Material,
            BuildLambertParameters("LambertVSRootParameters")
        });
        registry.Register({
            "LambertPS",
            "App/Assets/Engine/Shaders/Lambert.hlsl",
            "PSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            ShaderTypeKind::Material,
            BuildLambertParameters("LambertPSRootParameters")
        });
        registry.Register({
            "StandardPBRVS",
            "App/Assets/Engine/Shaders/StandardPBR.hlsl",
            "VSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Vertex,
            ShaderTypeKind::Material,
            BuildStandardPBRParameters("StandardPBRVSRootParameters")
        });
        registry.Register({
            "StandardPBRPS",
            "App/Assets/Engine/Shaders/StandardPBR.hlsl",
            "PSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            ShaderTypeKind::Material,
            BuildStandardPBRParameters("StandardPBRPSRootParameters")
        });
        registry.Register({
            "DeferredGBufferVS",
            "App/Assets/Engine/Shaders/DeferredGBuffer.hlsl",
            "VSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Vertex,
            ShaderTypeKind::Material,
            BuildDeferredGBufferParameters("DeferredGBufferVSRootParameters")
        });
        registry.Register({
            "DeferredGBufferPS",
            "App/Assets/Engine/Shaders/DeferredGBuffer.hlsl",
            "PSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            ShaderTypeKind::Material,
            BuildDeferredGBufferParameters("DeferredGBufferPSRootParameters")
        });
        registry.Register({
            "DeferredLightingPS",
            "App/Assets/Engine/Shaders/DeferredLighting.hlsl",
            "PSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            ShaderTypeKind::Global,
            BuildDeferredLightingParameters("DeferredLightingPSRootParameters")
        });
        registry.Register({
            "LightGridResetCS",
            "App/Assets/Engine/Shaders/LightGridReset.hlsl",
            "CSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Compute,
            ShaderTypeKind::Global,
            BuildLightGridResetParameters()
        });
        registry.Register({
            "LightGridInjectionCS",
            "App/Assets/Engine/Shaders/LightGridInjection.hlsl",
            "CSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Compute,
            ShaderTypeKind::Global,
            BuildLightGridInjectionParameters()
        });
        registry.Register({
            "LightGridCompactCS",
            "App/Assets/Engine/Shaders/LightGridCompact.hlsl",
            "CSMain",
            NLS::Render::ShaderCompiler::ShaderStage::Compute,
            ShaderTypeKind::Global,
            BuildLightGridCompactParameters()
        });
        return registry;
    }
}

namespace NLS::Render::Resources
{
    ShaderType::ShaderType(
        std::string name,
        std::string sourcePath,
        std::string entryPoint,
        ShaderCompiler::ShaderStage stage,
        ShaderTypeKind kind,
        ShaderRootParameterMetadata rootParameterMetadata,
        ShouldCompilePredicate shouldCompile)
        : m_name(std::move(name))
        , m_sourcePath(NormalizeShaderPath(std::move(sourcePath)))
        , m_entryPoint(std::move(entryPoint))
        , m_stage(stage)
        , m_kind(kind)
        , m_rootParameterMetadata(std::move(rootParameterMetadata))
        , m_shouldCompile(std::move(shouldCompile))
    {
    }

    std::string_view ShaderType::GetName() const
    {
        return m_name;
    }

    std::string_view ShaderType::GetSourcePath() const
    {
        return m_sourcePath;
    }

    std::string_view ShaderType::GetEntryPoint() const
    {
        return m_entryPoint;
    }

    ShaderCompiler::ShaderStage ShaderType::GetStage() const
    {
        return m_stage;
    }

    ShaderTypeKind ShaderType::GetKind() const
    {
        return m_kind;
    }

    const std::vector<ShaderParameterStruct>& ShaderType::GetRootParameterStructs() const
    {
        return m_rootParameterMetadata.groups;
    }

    const ShaderRootParameterMetadata* ShaderType::GetRootParameterMetadata() const
    {
        return &m_rootParameterMetadata;
    }

    bool ShaderType::ShouldCompilePermutation(const ShaderPermutationParameters& parameters) const
    {
        return m_shouldCompile ? m_shouldCompile(parameters) : true;
    }

    void ShaderTypeRegistry::Register(ShaderType shaderType)
    {
        m_shaderTypes.push_back(std::move(shaderType));
    }

    const ShaderType* ShaderTypeRegistry::FindByName(std::string_view name) const
    {
        const auto found = std::find_if(
            m_shaderTypes.begin(),
            m_shaderTypes.end(),
            [name](const ShaderType& shaderType)
            {
                return shaderType.GetName() == name;
            });
        return found != m_shaderTypes.end() ? &*found : nullptr;
    }

    std::vector<const ShaderType*> ShaderTypeRegistry::FindBySourcePath(std::string_view sourcePath) const
    {
        std::vector<const ShaderType*> matches;
        for (const auto& shaderType : m_shaderTypes)
        {
            if (PathMatchesShaderTypeSource(sourcePath, shaderType.GetSourcePath()))
                matches.push_back(&shaderType);
        }
        return matches;
    }

    const std::vector<ShaderType>& ShaderTypeRegistry::GetShaderTypes() const
    {
        return m_shaderTypes;
    }

    const ShaderTypeRegistry& GetShaderTypeRegistry()
    {
        static const ShaderTypeRegistry registry = BuildEngineShaderTypeRegistry();
        return registry;
    }
}
