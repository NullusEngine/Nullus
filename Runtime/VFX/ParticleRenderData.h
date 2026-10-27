#pragma once

#include <optional>

#include "VfxProgram.h"
#include "ParticleBounds.h"
#include <Rendering/RHI/Core/RHIDevice.h>

namespace NLS::VFX
{
// Frozen per-world-frame render storage. Views keep it alive independently of the simulator.
struct ParticleRenderData
{
    std::shared_ptr<Render::RHI::RHIDevice> device;
    std::vector<Attribute> attributes;
    uint32_t capacity = 0;
    uint32_t countUpperBound = 0;
    std::shared_ptr<Render::RHI::RHIBuffer> values, ids, counters;
    std::shared_ptr<Render::RHI::RHIBuffer> bounds;
    std::optional<ParticleBounds> cpuBounds;
    // CPU-visible conservative bounds for scene-level ordering. CPU emitters
    // publish their frame bounds; GPU emitters publish the latest completed
    // asynchronous readback, which can trail the simulation by a few frames.
    std::optional<ParticleBounds> orderingBounds;
    uint64_t orderingBoundsFrame = 0;
};

NLS_VFX_API std::shared_ptr<const ParticleRenderData> UploadCpuRenderData(
    const std::shared_ptr<Render::RHI::RHIDevice>& device, const ParticleDataSet& data, std::string& error,
    const ParticleBounds* bounds = nullptr);
}
