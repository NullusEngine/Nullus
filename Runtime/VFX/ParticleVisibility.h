#pragma once

#include "ParticleRenderData.h"
#include "VfxShaders.h"
#include "MeshRenderGeometry.h"
#include <Rendering/Context/RenderScenePackage.h>

namespace NLS::VFX
{
struct GpuBoundsResources
{
    std::shared_ptr<Render::RHI::RHIDevice> device;
    std::shared_ptr<Render::RHI::RHIBuffer> bounds, partials, constants;
    std::vector<Render::Context::RecordedComputeDispatchInput> dispatches;
};
struct ParticleVisibilityResources
{
    std::shared_ptr<Render::RHI::RHIDevice> device;
    std::shared_ptr<Render::RHI::RHIBuffer> keys, arguments, scratch, groups, constants, bounds;
    std::vector<Render::Context::RecordedComputeDispatchInput> dispatches;
};

class NLS_VFX_API ParticleBoundsCalculator final
{
public:
    bool Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const std::vector<Attribute>& attributes,
        const GpuBoundsShaders& shaders, std::string& error, const MeshSettings* mesh = nullptr);
    [[nodiscard]] std::shared_ptr<const GpuBoundsResources> Prepare(const ParticleRenderData& data, std::string& error,
        const std::vector<Value>& meshBounds = {}) const;
private:
    std::shared_ptr<Render::RHI::RHIDevice> m_device;
    std::shared_ptr<Render::RHI::RHIBindingLayout> m_layout;
    std::shared_ptr<Render::RHI::RHIComputePipeline> m_reduce, m_finish;
    std::vector<Attribute> m_attributes;
};

class NLS_VFX_API ParticleVisibility final
{
public:
    bool Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const std::vector<Attribute>& attributes,
        const GpuVisibilityShaders& shaders, std::string& error, const MeshSettings* mesh = nullptr);
    [[nodiscard]] std::shared_ptr<const ParticleVisibilityResources> Prepare(const ParticleRenderData& data,
        const SpriteView& view, std::string& error, const MeshDrawInfo* mesh = nullptr) const;
private:
    std::shared_ptr<Render::RHI::RHIDevice> m_device;
    std::shared_ptr<Render::RHI::RHIBindingLayout> m_layout;
    std::shared_ptr<Render::RHI::RHIComputePipeline> m_cull, m_scan, m_scatter;
    std::vector<Attribute> m_attributes;
};
}
