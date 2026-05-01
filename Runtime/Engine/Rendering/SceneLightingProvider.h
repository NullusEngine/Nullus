#pragma once

#include <Rendering/Context/ThreadedRenderingLifecycle.h>
#include <Rendering/Data/LightingDescriptor.h>

#include "EngineDef.h"

namespace NLS::Engine::SceneSystem
{
    class Scene;
}

namespace NLS::Engine::Rendering
{
class NLS_ENGINE_API SceneLightingProvider
{
public:
    using LightingDescriptor = NLS::Render::Data::LightingDescriptor;
    using LightSet = NLS::Render::Data::LightSet;

    void Collect(const SceneSystem::Scene& scene);
    void PrepareRenderScenePackage(
        const NLS::Render::Context::FrameSnapshot& snapshot,
        NLS::Render::Context::RenderScenePackage& package) const;
    const LightingDescriptor& GetLightingDescriptor() const;

private:
    LightingDescriptor m_lightingDescriptor;
};
}
