#pragma once

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
    const LightingDescriptor& GetLightingDescriptor() const;

private:
    LightingDescriptor m_lightingDescriptor;
};
}
