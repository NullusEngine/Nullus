#pragma once

#include <algorithm>
#include <any>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <type_traits>
#include <vector>

#include "Object/Object.h"
#include "Reflection/Macros.h"
#include "Rendering/Data/StateMask.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/Resources/MaterialLayout.h"
#include "Rendering/Resources/MaterialParameterBlock.h"
#include "Rendering/Resources/ResourceBinding.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/ECullFace.h"
#include "Rendering/Settings/EPrimitiveMode.h"
#include "Rendering/ShaderLab/ShaderLabTypes.h"
#include "Rendering/ShaderLab/ShaderLabVariant.h"
#include "Resources/Material.generated.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
    class MaterialResourceSet;
    struct MaterialPipelineStateOverrides;
    enum class MaterialSurfaceMode : uint8_t;
    class Material;
    class Shader;
    struct ShaderPropertyDesc;
    class Texture2D;
}
namespace NLS::Render::RHI
{
    class RHIDevice;
    class RHIBindingLayout;
    class RHIBindingSet;
    class PipelineCache;
    class RHIPipelineLayout;
    class RHIGraphicsPipeline;
}

namespace NLS::Render::Context
{
class Driver;
}

namespace NLS::Render::Core
{
class ABaseRenderer;
}

namespace NLS::Render::Resources
{
struct MaterialPipelineStateOverrides
{
    static constexpr size_t kInlineColorFormatCapacity = 4u;
    static constexpr size_t kInlineColorSpaceCapacity = 4u;
    static constexpr size_t kInlineRenderTargetBlendStateCapacity = 4u;
    std::optional<bool> depthWrite, colorWrite, blending, depthTest, hasDepthAttachment, culling, stencilTest;
    std::optional<bool> alphaToCoverage, independentBlend;
    std::optional<uint32_t> stencilWriteMask, sampleCount;
    std::optional<Settings::EComparaisonAlgorithm> depthCompare;
    std::optional<Settings::ECullFace> cullFace;
    std::optional<RHI::TextureFormat> depthFormat;
    std::optional<std::vector<RHI::TextureFormat>> colorFormats;
    std::optional<std::vector<RHI::TextureColorSpace>> colorSpaces;
    std::optional<std::vector<RHI::RHIRenderTargetBlendStateDesc>> renderTargetBlendStates;
    std::array<RHI::TextureFormat, kInlineColorFormatCapacity> inlineColorFormats {};
    std::array<RHI::TextureColorSpace, kInlineColorSpaceCapacity> inlineColorSpaces {};
    std::array<RHI::RHIRenderTargetBlendStateDesc, kInlineRenderTargetBlendStateCapacity> inlineRenderTargetBlendStates {};
    size_t inlineColorFormatCount = 0u;
    size_t inlineColorSpaceCount = 0u;
    size_t inlineRenderTargetBlendStateCount = 0u;
    bool inlineColorFormatsSet = false;
    bool inlineColorSpacesSet = false;
    bool inlineRenderTargetBlendStatesSet = false;
    void SetColorFormats(std::span<const RHI::TextureFormat> formats)
    {
        inlineColorFormatCount = 0u;
        inlineColorFormatsSet = true;
        colorFormats.reset();
        if (formats.size() <= inlineColorFormats.size())
        {
            for (size_t i = 0u; i < formats.size(); ++i)
                inlineColorFormats[i] = formats[i];
            inlineColorFormatCount = formats.size();
            return;
        }

        colorFormats = std::vector<RHI::TextureFormat>(formats.begin(), formats.end());
        inlineColorFormatsSet = false;
    }

    bool HasColorFormatsOverride() const { return inlineColorFormatsSet || colorFormats.has_value(); }

    void SetColorSpaces(std::span<const RHI::TextureColorSpace> spaces)
    {
        inlineColorSpaceCount = 0u;
        inlineColorSpacesSet = true;
        colorSpaces.reset();
        if (spaces.size() <= inlineColorSpaces.size())
        {
            for (size_t i = 0u; i < spaces.size(); ++i)
                inlineColorSpaces[i] = spaces[i];
            inlineColorSpaceCount = spaces.size();
            return;
        }

        colorSpaces = std::vector<RHI::TextureColorSpace>(spaces.begin(), spaces.end());
        inlineColorSpacesSet = false;
    }

    bool HasColorSpacesOverride() const { return inlineColorSpacesSet || colorSpaces.has_value(); }

    void SetRenderTargetBlendStates(std::span<const RHI::RHIRenderTargetBlendStateDesc> states)
    {
        inlineRenderTargetBlendStateCount = 0u;
        inlineRenderTargetBlendStatesSet = true;
        renderTargetBlendStates.reset();
        if (states.size() <= inlineRenderTargetBlendStates.size())
        {
            for (size_t i = 0u; i < states.size(); ++i)
                inlineRenderTargetBlendStates[i] = states[i];
            inlineRenderTargetBlendStateCount = states.size();
            return;
        }

        renderTargetBlendStates = std::vector<RHI::RHIRenderTargetBlendStateDesc>(states.begin(), states.end());
        inlineRenderTargetBlendStatesSet = false;
    }

    bool HasRenderTargetBlendStatesOverride() const
    {
        return inlineRenderTargetBlendStatesSet || renderTargetBlendStates.has_value();
    }

    std::span<const RHI::RHIRenderTargetBlendStateDesc> GetRenderTargetBlendStates() const
    {
        if (renderTargetBlendStates.has_value())
            return std::span<const RHI::RHIRenderTargetBlendStateDesc>(*renderTargetBlendStates);
        if (inlineRenderTargetBlendStatesSet)
            return std::span<const RHI::RHIRenderTargetBlendStateDesc>(
                inlineRenderTargetBlendStates.data(),
                inlineRenderTargetBlendStateCount);
        return {};
    }

    std::span<const RHI::TextureFormat> GetColorFormats() const
    {
        if (colorFormats.has_value())
            return std::span<const RHI::TextureFormat>(*colorFormats);
        if (inlineColorFormatsSet)
            return std::span<const RHI::TextureFormat>(inlineColorFormats.data(), inlineColorFormatCount);
        return {};
    }

    std::span<const RHI::TextureColorSpace> GetColorSpaces() const
    {
        if (colorSpaces.has_value())
            return std::span<const RHI::TextureColorSpace>(*colorSpaces);
        if (inlineColorSpacesSet)
            return std::span<const RHI::TextureColorSpace>(inlineColorSpaces.data(), inlineColorSpaceCount);
        return {};
    }

    friend bool operator==(const MaterialPipelineStateOverrides& lhs, const MaterialPipelineStateOverrides& rhs)
    {
        return lhs.depthWrite == rhs.depthWrite &&
            lhs.colorWrite == rhs.colorWrite &&
            lhs.blending == rhs.blending &&
            lhs.depthTest == rhs.depthTest &&
            lhs.hasDepthAttachment == rhs.hasDepthAttachment &&
            lhs.culling == rhs.culling &&
            lhs.stencilTest == rhs.stencilTest &&
            lhs.alphaToCoverage == rhs.alphaToCoverage &&
            lhs.independentBlend == rhs.independentBlend &&
            lhs.stencilWriteMask == rhs.stencilWriteMask &&
            lhs.sampleCount == rhs.sampleCount &&
            lhs.depthCompare == rhs.depthCompare &&
            lhs.cullFace == rhs.cullFace &&
            lhs.depthFormat == rhs.depthFormat &&
            lhs.HasColorFormatsOverride() == rhs.HasColorFormatsOverride() &&
            lhs.GetColorFormats().size() == rhs.GetColorFormats().size() &&
            std::equal(
                lhs.GetColorFormats().begin(),
                lhs.GetColorFormats().end(),
                rhs.GetColorFormats().begin()) &&
            lhs.HasColorSpacesOverride() == rhs.HasColorSpacesOverride() &&
            lhs.GetColorSpaces().size() == rhs.GetColorSpaces().size() &&
            std::equal(
                lhs.GetColorSpaces().begin(),
                lhs.GetColorSpaces().end(),
                rhs.GetColorSpaces().begin()) &&
            lhs.HasRenderTargetBlendStatesOverride() == rhs.HasRenderTargetBlendStatesOverride() &&
            lhs.GetRenderTargetBlendStates().size() == rhs.GetRenderTargetBlendStates().size() &&
            std::equal(
                lhs.GetRenderTargetBlendStates().begin(),
                lhs.GetRenderTargetBlendStates().end(),
                rhs.GetRenderTargetBlendStates().begin());
    }

    friend bool operator!=(const MaterialPipelineStateOverrides& lhs, const MaterialPipelineStateOverrides& rhs)
    {
        return !(lhs == rhs);
    }

    NLS_RENDER_API size_t GetHash() const;
};

NLS_RENDER_API MaterialPipelineStateOverrides BuildMaterialPipelineStateOverrides(
    const NLS::Render::ShaderLab::ShaderLabPassState& state);

enum class MaterialBindingDiagnosticSeverity : uint8_t
{
    Info,
    Warning,
    Error
};

struct MaterialBindingDiagnostic
{
    MaterialBindingDiagnosticSeverity severity = MaterialBindingDiagnosticSeverity::Info;
    std::string bindingName;
    std::string message;
};

/**
 * A material is a combination of a shader and some settings (Material settings and shader settings)
 */
CLASS(NLS_RENDER_API Material) : public NLS::NamedObject
{
public:
    GENERATED_BODY()

    using ShaderType = Shader;
    using Texture2DType = Texture2D;

    /**
     * Creates a material
     * @param p_shader
     */
    Material(Shader* p_shader = nullptr);
    ~Material();

    /**
     * Defines the shader to attach to this material instance
     * @param p_shader
     */
    void SetShader(Shader* p_shader);

    /**
     * Fill uniform with default uniform values
     */
    void FillUniform();

    void SyncParameterLayout();

    void RebuildBindingLayout();
    void RebuildBindingSet() const;
    void RebuildBindingSet(const Shader* effectiveShader) const;

    /**
     * Set a shader uniform value
     * @param p_key
     * @param p_value
     */
    template<typename T>
    void Set(const std::string& p_key, const T& p_value);

    /**
     * Set a shader uniform value
     * @param p_key
     */
    template<typename T>
    const T& Get(const std::string p_key) const;

    /**
     * Returns the attached shader
     */
    Shader*& GetShader();
    const Shader* GetShader() const;
    Shader* ResolveShaderForLightMode(std::string_view lightMode) const;
    void SetShaderLabSourcePath(std::string sourcePath);
    const std::string& GetShaderLabSourcePath() const;
    bool HasExplicitShaderLabSourcePath() const;
    void SetShaderReferencePath(std::string shaderPath);
    const std::string& GetShaderReferencePath() const;
    void RegisterShaderLabPassShader(Shader* shader);
    void ClearShaderReferences(const Shader* shader);
    static void ClearShaderReferencesFromLiveMaterials(const Shader* shader);

    /**
     * Returns true if the material has a shader attached
     */
    bool HasShader() const;

    /**
     * Returns true if the material is valid
     */
    bool IsValid() const;

    /**
     * Defines if the material is blendable for legacy opaque/transparent callers.
     * Decal materials stay blendable; call SetSurfaceMode(Opaque) or
     * SetSurfaceMode(Transparent) to leave Decal mode.
     * @param p_blendable
     */
    void SetBlendable(bool p_blendable);
    void SetSurfaceMode(MaterialSurfaceMode surfaceMode);

    /**
     * Defines if the material has backface culling
     * @param p_backfaceCulling
     */
    void SetBackfaceCulling(bool p_backfaceCulling);

    /**
     * Defines if the material has frontface culling
     * @param p_frontfaceCulling
     */
    void SetFrontfaceCulling(bool p_frontfaceCulling);

    /**
     * Defines if the material has depth test
     * @param p_depthTest
     */
    void SetDepthTest(bool p_depthTest);

    /**
     * Defines if the material has depth writting
     * @param p_depthWriting
     */
    void SetDepthWriting(bool p_depthWriting);

    /**
     * Defines if the material has color writting
     * @param p_colorWriting
     */
    void SetColorWriting(bool p_colorWriting);

    /**
     * Defines the number of instances
     * @param p_instances
     */
    void SetGPUInstances(int p_instances);
    void EnableKeyword(std::string keyword);
    void DisableKeyword(std::string_view keyword);
    bool IsKeywordEnabled(std::string_view keyword) const;
    std::vector<std::string> GetShaderLabKeywordNames() const;
    const NLS::Render::ShaderLab::ShaderLabKeywordSet& GetShaderLabKeywords() const;

    /**
     * Returns true if the material is blendable
     */
    bool IsBlendable() const;
    MaterialSurfaceMode GetSurfaceMode() const;
    bool IsDecal() const;
    bool IsTransparentSurface() const;

    /**
     * Returns true if the material has backface culling
     */
    bool HasBackfaceCulling() const;

    /**
     * Returns true if the material has frontface culling
     */
    bool HasFrontfaceCulling() const;

    /**
     * Returns true if the material has depth test
     */
    bool HasDepthTest() const;

    /**
     * Returns true if the material has depth writing
     */
    bool HasDepthWriting() const;

    /**
     * Returns true if the material has color writing
     */
    bool HasColorWriting() const;

    /**
     * Returns the number of instances
     */
    int GetGPUInstances() const;

    /**
     * Generate a state mask with the current material settings
     */
    const Data::StateMask GenerateStateMask() const;

    const MaterialParameterBlock& GetParameterBlock() const;

    /**
     * Returns the uniforms data of the material
     */
    const std::map<std::string, std::any>& GetUniformsData() const;

    struct MaterialRuntimeState;  // Forward declaration for public methods

    // Formal RHI methods - exposed for renderer direct access
    std::shared_ptr<RHI::RHIGraphicsPipeline> BuildRecordedGraphicsPipeline(
        const std::shared_ptr<RHI::RHIDevice>& device,
        const std::shared_ptr<RHI::PipelineCache>& pipelineCache,
        Settings::EPrimitiveMode primitiveMode,
        const Data::PipelineState& pipelineState,
        MaterialPipelineStateOverrides overrides = {},
        bool* hasPipelineLayout = nullptr,
        bool* hasVertexShader = nullptr,
        bool* hasFragmentShader = nullptr,
        const Shader* effectiveShader = nullptr) const;
    MaterialRuntimeState& GetRuntimeState() const;
    void ResetRuntimeState() const;
    void EnsureShaderGenerationCacheCurrent() const;
    void InvalidateExplicitBindingSetCache() const;
    std::shared_ptr<RHI::RHIBindingSet> GetRecordedBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const;
    std::shared_ptr<RHI::RHIBindingSet> GetRecordedBindingSet(
        const std::shared_ptr<RHI::RHIDevice>& device,
        const Shader* effectiveShader) const;
    const MaterialResourceSet& GetBindingSet() const;
    const std::shared_ptr<RHI::RHIBindingLayout>& GetExplicitBindingLayout(const std::shared_ptr<RHI::RHIDevice>& device) const;
    const std::shared_ptr<RHI::RHIBindingLayout>& GetExplicitBindingLayout(
        const std::shared_ptr<RHI::RHIDevice>& device,
        const Shader* effectiveShader) const;
    const std::shared_ptr<RHI::RHIBindingSet>& GetExplicitBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const;
    const std::shared_ptr<RHI::RHIBindingSet>& GetExplicitBindingSet(
        const std::shared_ptr<RHI::RHIDevice>& device,
        const Shader* effectiveShader) const;
    const std::shared_ptr<RHI::RHIPipelineLayout>& GetExplicitPipelineLayout(const std::shared_ptr<RHI::RHIDevice>& device) const;
    const std::shared_ptr<RHI::RHIPipelineLayout>& GetExplicitPipelineLayout(
        const std::shared_ptr<RHI::RHIDevice>& device,
        const Shader* effectiveShader) const;
    const std::vector<MaterialBindingDiagnostic>& GetLastExplicitBindingDiagnostics() const;
    bool HasExplicitBindingErrors() const;
    bool RequiresPassDescriptorSet() const;
    bool RequiresPassDescriptorSet(const Shader* effectiveShader) const;
    void SetRawParameter(const std::string& name, std::any value);
    void MarkParametersDirty();
#if defined(NLS_ENABLE_TEST_HOOKS)
    struct ExplicitShaderCacheEntryCountsForTesting
    {
        size_t bindingLayouts = 0u;
        size_t bindingSets = 0u;
        size_t pipelineLayouts = 0u;
    };

    uint64_t GetCachedShaderGenerationForTesting() const;
    uint64_t GetIndexedObjectDataShaderValidationCountForTesting() const;
    ExplicitShaderCacheEntryCountsForTesting GetExplicitShaderCacheEntryCountsForTesting(
        uint64_t shaderInstanceId,
        uint64_t shaderGeneration) const;
    size_t GetExplicitBindingDiagnosticCountForTesting(
        uint64_t shaderInstanceId,
        uint64_t shaderGeneration) const;
#endif
    void SetTextureResourcePath(const std::string& name, std::string path);
    void ClearTextureResourcePath(const std::string& name);
    std::string GetTextureResourcePath(const std::string& name) const;
    const std::map<std::string, std::string>& GetTextureResourcePaths() const;
    void SetSamplerOverride(const std::string& name, const RHI::SamplerDesc& sampler);
    void ClearSamplerOverride(const std::string& name);
    void ClearSamplerOverrides();
    const RHI::SamplerDesc* GetSamplerOverride(const std::string& name) const;
    const std::map<std::string, RHI::SamplerDesc>& GetSamplerOverrides() const;
    uint64_t GetInstanceId() const;
    uint64_t GetParameterRevision() const;
    uint64_t GetRenderStateRevision() const;
    uint64_t GetBindingRevision() const;
    uint64_t GetExplicitBindingSetCreationCount() const;
    uint64_t GetExplicitSnapshotBufferCreationCount() const;

    std::string path;

protected:
    std::optional<ShaderPropertyDesc> FindMaterialProperty(const std::string& key) const;
    bool EnsureMaterialParameterExists(const std::string& key);

    Shader* m_shader = nullptr;
    std::string m_shaderLabSourcePath;
    bool m_shaderLabSourcePathExplicit = false;
    std::string m_shaderReferencePath;
    std::unordered_map<std::string, Shader*> m_shaderLabPassShadersByLightMode;
    MaterialParameterBlock m_parameterBlock;
    std::map<std::string, std::string> m_textureResourcePaths;
    std::map<std::string, RHI::SamplerDesc> m_samplerOverrides;
    NLS::Render::ShaderLab::ShaderLabKeywordSet m_shaderLabKeywords;
    MaterialLayout m_materialLayout;
    ResourceBindingLayout m_bindingLayout;
    mutable std::unique_ptr<MaterialRuntimeState> m_runtimeState;

    bool m_blendable = false;
    MaterialSurfaceMode m_surfaceMode;
    bool m_backfaceCulling = true;
    bool m_frontfaceCulling = false;
    bool m_depthTest = true;
    bool m_depthWriting = true;
    bool m_colorWriting = true;
    int m_gpuInstances = 1;
    uint64_t m_instanceId = 0u;
    uint64_t m_renderStateRevision = 1u;
    mutable uint64_t m_bindingRevision = 1u;

private:
    void PruneStaleExplicitShaderGenerationEntries(
        uint64_t shaderInstanceId,
        uint64_t shaderGeneration) const;

    friend class NLS::Render::Context::Driver;
    friend class NLS::Render::Core::ABaseRenderer;
};

enum class MaterialSurfaceMode : uint8_t
{
    Opaque = 0,
    Transparent,
    Decal
};

NLS_RENDER_API const char* MaterialSurfaceModeName(MaterialSurfaceMode mode);
NLS_RENDER_API std::optional<MaterialSurfaceMode> ParseMaterialSurfaceMode(const std::string& value);
NLS_RENDER_API bool MaterialIdentitySuggestsDecal(
    std::string_view displayName,
    std::string_view sourceSubAsset);
} // namespace NLS::Render::Resources

#include "Rendering/Resources/Material.inl"
