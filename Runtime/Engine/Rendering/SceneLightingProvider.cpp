#include "Rendering/SceneLightingProvider.h"

#include "Components/LightComponent.h"
#include "SceneSystem/Scene.h"
#include "Rendering/SceneRenderStore.h"

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

void SceneLightingProvider::CollectFromStore(const SceneRenderStore& store)
{
    m_lightingDescriptor.lights.clear();
    store.ForEachLight(
        [this](const NLS::Render::Entities::Light& light)
        {
            m_lightingDescriptor.lights.push_back(std::cref(light));
        });
}

void SceneLightingProvider::PrepareRenderScenePackage(
    const NLS::Render::Context::FrameSnapshot& snapshot,
    NLS::Render::Context::RenderScenePackage& package) const
{
    package.hasLightingData = snapshot.sceneLightCount > 0u;
    package.lightingDataReady = true;
}

const SceneLightingProvider::LightingDescriptor& SceneLightingProvider::GetLightingDescriptor() const
{
    return m_lightingDescriptor;
}
}
