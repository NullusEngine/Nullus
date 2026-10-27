#pragma once

#include "EngineDef.h"
#include <Math/Matrix4.h>
#include <Rendering/Context/RenderScenePackage.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <VFX/SystemRenderProxy.h>
#include <VFX/SpriteRenderer.h>

namespace NLS::Engine::Rendering
{
struct ParticleViewInput
{
    std::shared_ptr<VFX::SystemRenderProxy> proxy;
    std::shared_ptr<const VFX::SystemFrame> frame;
    Maths::Matrix4 localToWorld = Maths::Matrix4::Identity;
    // Per emitter, per mesh slot; the sprite path stores one entry per emitter.
    std::vector<std::vector<VFX::SpriteMaterial>> materials;
    std::vector<std::vector<std::shared_ptr<const VFX::MeshRenderGeometry>>> meshes;
    std::string materialError;
};

class NLS_ENGINE_API ParticleSceneRenderer final
{
public:
    ParticleSceneRenderer();
    ~ParticleSceneRenderer();
    ParticleSceneRenderer(const ParticleSceneRenderer&) = delete;
    ParticleSceneRenderer& operator=(const ParticleSceneRenderer&) = delete;
    bool AppendPass(std::shared_ptr<Render::RHI::RHIDevice> device, Render::Context::RenderScenePackage& package,
        const Render::FrameGraph::FrozenFrameDescriptor& view, const std::vector<ParticleViewInput>& particles, std::string& error);
private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
