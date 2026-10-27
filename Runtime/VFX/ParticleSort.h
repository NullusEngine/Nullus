#pragma once

#include "ParticleRenderData.h"
#include "VfxShaders.h"
#include <Rendering/Context/RenderScenePackage.h>

namespace NLS::VFX
{
// Per-view permutation. The source SoA, IDs and indirect count remain immutable.
struct ParticleSortResources
{
    std::shared_ptr<Render::RHI::RHIDevice> device;
    std::shared_ptr<Render::RHI::RHIBuffer> keys, constants;
    std::vector<Render::Context::RecordedComputeDispatchInput> dispatches;
};

class NLS_VFX_API ParticleSorter final
{
public:
    bool Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const std::vector<Attribute>& attributes,
        const GpuSortShaders& shaders, std::string& error);
    [[nodiscard]] std::shared_ptr<const ParticleSortResources> Prepare(const ParticleRenderData& data,
        const std::array<float,16>& localToWorld, const Value& cameraForward, std::string& error,
        std::shared_ptr<Render::RHI::RHIBuffer> initializedKeys = {}) const;
    static bool Record(Render::RHI::RHICommandBuffer& commands, const ParticleSortResources& resources, std::string& error);

private:
    std::shared_ptr<Render::RHI::RHIDevice> m_device;
    std::shared_ptr<Render::RHI::RHIBindingLayout> m_layout;
    std::shared_ptr<Render::RHI::RHIComputePipeline> m_initialize, m_step;
    std::vector<Attribute> m_attributes;
    size_t m_scalars = 0;
};
}
