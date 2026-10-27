#pragma once

#include "GpuDataSet.h"
#include "ParticleSort.h"
#include "ParticleVisibility.h"
#include "SpriteMaterial.h"

namespace NLS::VFX
{
struct ParticleDrawResources
{
    struct MaterialBinding
    {
        std::shared_ptr<Render::RHI::RHIGraphicsPipeline> pipeline;
        std::shared_ptr<Render::RHI::RHIBindingSet> bindings;
        std::vector<std::shared_ptr<Render::RHI::RHITextureView>> textures;
        std::vector<std::shared_ptr<Render::RHI::RHIBuffer>> constants;
        std::vector<std::shared_ptr<Render::RHI::RHISampler>> samplers;
    };
    struct MeshSlot
    {
        std::shared_ptr<const MeshRenderGeometry> geometry;
        std::shared_ptr<Render::RHI::RHIMesh> mesh;
        uint32_t indexCount = 0;
        uint64_t argumentOffset = 0;
        std::shared_ptr<Render::RHI::RHIBuffer> constants;
        std::shared_ptr<Render::RHI::RHIBindingSet> bindings;
        std::shared_ptr<Render::RHI::RHITextureView> texture;
        std::shared_ptr<Render::RHI::RHISampler> sampler;
        MaterialBinding material;
    };
    std::shared_ptr<Render::RHI::RHIDevice> device;
    std::shared_ptr<Render::RHI::RHIBuffer> constants, attributes, arguments;
    std::shared_ptr<Render::RHI::RHIBindingSet> bindings;
    std::shared_ptr<Render::RHI::RHIGraphicsPipeline> pipeline;
    std::shared_ptr<const ParticleSortResources> sorting;
    std::shared_ptr<const GpuBoundsResources> calculatedBounds;
    std::shared_ptr<const ParticleVisibilityResources> visibility;
    std::vector<Render::Context::RecordedComputeDispatchInput> dispatches;
    std::shared_ptr<Render::RHI::RHITextureView> texture, sceneDepth;
    std::shared_ptr<Render::RHI::RHISampler> sampler;
    std::shared_ptr<Render::RHI::RHIBindingSet> materialBindings;
    std::vector<std::shared_ptr<Render::RHI::RHITextureView>> materialTextures;
    std::vector<std::shared_ptr<Render::RHI::RHIBuffer>> materialConstants;
    std::vector<std::shared_ptr<Render::RHI::RHISampler>> materialSamplers;
    bool usesSceneDepth = false;
    std::vector<MeshSlot> meshSlots;
};
using SpriteDrawResources = ParticleDrawResources;

// Shared material, visibility, sorting and submission path for particle geometry.
class NLS_VFX_API ParticleRenderer
{
public:
    [[nodiscard]] bool WritesDepth() const { return m_settings.blend == SpriteBlendMode::Masked || m_settings.blend == SpriteBlendMode::Opaque; }
    bool Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const std::vector<Attribute>& attributes,
        const Render::RHI::RHIShaderModuleDesc& vertex, const Render::RHI::RHIShaderModuleDesc& fragment,
        const Render::RHI::RHIRenderTargetLayoutDesc& targets, std::string& error, const GpuSortShaders* sorting = nullptr,
        const GpuVisibilityShaders* visibility = nullptr, const GpuBoundsShaders* bounds = nullptr,
        const SpriteSettings& settings = {}, const MeshSettings* mesh = nullptr);
    [[nodiscard]] std::shared_ptr<const SpriteDrawResources> Prepare(
        const ParticleRenderData& data, const SpriteView& view, std::string& error, const SpriteDrawContext& context = {}) const;
    [[nodiscard]] std::shared_ptr<const SpriteDrawResources> Record(Render::RHI::RHICommandBuffer& commands,
        const GpuDataSet& data, const SpriteView& view, const Render::RHI::RHIRenderPassDesc& pass, std::string& error,
        const SpriteDrawContext& context = {}) const;
    [[nodiscard]] std::shared_ptr<const SpriteDrawResources> Record(Render::RHI::RHICommandBuffer& commands,
        const ParticleRenderData& data, const SpriteView& view, const Render::RHI::RHIRenderPassDesc& pass, std::string& error,
        const SpriteDrawContext& context = {}) const;

private:
    bool PrepareMaterial(ParticleDrawResources::MaterialBinding& binding, const SpriteMaterial& material, std::string& error) const;
    struct MaterialPipeline
    {
        std::shared_ptr<const SpriteMaterialProgram> program;
        std::shared_ptr<Render::RHI::RHIBindingLayout> layout;
        std::shared_ptr<Render::RHI::RHIGraphicsPipeline> pipeline;
    };
    mutable std::vector<MaterialPipeline> m_materialPipelines;
    std::shared_ptr<Render::RHI::RHIDevice> m_device;
    std::shared_ptr<Render::RHI::RHIBindingLayout> m_layout;
    std::shared_ptr<Render::RHI::RHIGraphicsPipeline> m_pipeline;
    std::vector<Attribute> m_attributes;
    std::shared_ptr<ParticleSorter> m_sorter;
    std::shared_ptr<ParticleBoundsCalculator> m_bounds;
    std::shared_ptr<ParticleVisibility> m_visibility;
    std::shared_ptr<Render::RHI::RHIBuffer> m_unsortedKey;
    std::shared_ptr<Render::RHI::RHITextureView> m_white, m_farDepth;
    SpriteSettings m_settings;
    bool m_meshRenderer = false;
    uint32_t m_meshSlotCount = 0;
    int32_t m_meshLodIndex = -1;
};
}
