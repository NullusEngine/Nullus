#include "Rendering/SceneLightingProvider.h"

#include "Components/LightComponent.h"
#include "SceneSystem/Scene.h"

namespace NLS::Engine::Rendering
{
void SceneLightingProvider::Collect(const SceneSystem::Scene& scene)
{
    m_lightingDescriptor.lights.clear();

    for (auto* light : scene.GetFastAccessComponents().lights)
    {
        if (!light)
            continue;

        auto* owner = light->gameobject();
        if (owner != nullptr && owner->IsActive())
            m_lightingDescriptor.lights.push_back(std::cref(*light->GetData()));
    }
}

const SceneLightingProvider::LightingDescriptor& SceneLightingProvider::GetLightingDescriptor() const
{
    return m_lightingDescriptor;
}
}
