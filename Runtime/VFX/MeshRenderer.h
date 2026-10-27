#pragma once

#include "ParticleRenderer.h"

namespace NLS::VFX
{
using MeshDrawResources = ParticleDrawResources;
class MeshRenderer final : public ParticleRenderer
{
public:
    bool Initialize(std::shared_ptr<Render::RHI::RHIDevice> device, const std::vector<Attribute>& attributes,
        const Render::RHI::RHIShaderModuleDesc& vertex, const Render::RHI::RHIShaderModuleDesc& fragment,
        const Render::RHI::RHIRenderTargetLayoutDesc& targets, std::string& error, const MeshSettings& mesh,
        const GpuSortShaders& sorting, const GpuVisibilityShaders& visibility, const GpuBoundsShaders& bounds,
        const SpriteSettings& material = {}, bool sort = false)
    { return ParticleRenderer::Initialize(std::move(device),attributes,vertex,fragment,targets,error,sort ? &sorting : nullptr,&visibility,&bounds,material,&mesh); }
};
}
