#include "Rendering/SceneLightingProvider.h"

#include "Components/LightComponent.h"
#include "SceneSystem/Scene.h"
#include "Rendering/SceneRenderStore.h"

namespace NLS::Engine::Rendering
{
void SceneLightingProvider::BeginCollection()
{
    m_lightingDescriptor.lights.clear();
    m_snapshotCount = 0u;
}

void SceneLightingProvider::AppendSnapshot(const NLS::Render::Entities::Light& source)
{
    const size_t index = m_snapshotCount;
    if (index >= m_snapshotTransforms.size())
    {
        m_snapshotTransforms.push_back(std::make_shared<NLS::Maths::Transform>());
        // The owned light must point at the owned transform, never at the
        // source's, so the snapshot survives the source's destruction.
        m_snapshotLights.push_back(std::make_shared<NLS::Render::Entities::Light>(
            m_snapshotTransforms.back().get()));
    }

    NLS::Maths::Transform& ownedTransform = *m_snapshotTransforms[index];
    NLS::Render::Entities::Light& ownedLight = *m_snapshotLights[index];

    if (source.transform != nullptr)
    {
        // Snapshot the world state rather than the local state: consumers read
        // the world position and forward directly.
        ownedTransform.GenerateMatricesWorld(
            source.transform->GetWorldPosition(),
            source.transform->GetWorldRotation(),
            source.transform->GetWorldScale());
    }

    ownedLight.color = source.color;
    ownedLight.intensity = source.intensity;
    ownedLight.range = source.range;
    ownedLight.size = source.size;
    ownedLight.cutoff = source.cutoff;
    ownedLight.outerCutoff = source.outerCutoff;
    ownedLight.type = source.type;

    m_lightingDescriptor.lights.push_back(std::cref(ownedLight));
    ++m_snapshotCount;
}

void SceneLightingProvider::Collect(const SceneSystem::Scene& scene)
{
    BeginCollection();

    for (auto* light : scene.GetFastAccessComponents().lights)
    {
        if (!light)
            continue;

        auto* owner = light->gameobject();
        if (owner == nullptr || !owner->IsActive())
            continue;

        // Scene components own their lights for as long as the scene lives, and
        // the published set is consumed within the same frame, so this path keeps
        // the live identity instead of snapshotting.
        if (const auto* source = light->GetData(); source != nullptr)
            m_lightingDescriptor.lights.push_back(std::cref(*source));
    }

    PublishLightingDescriptor();
}

void SceneLightingProvider::CollectFromStore(const SceneRenderStore& store)
{
    // Snapshot: store slots destroy their lights on Remove/Clear and one store is
    // shared by every render scene of that Scene, so referencing the store here
    // would let a removal in one view dangle another view's published set.
    BeginCollection();
    store.ForEachLight(
        [this](const NLS::Render::Entities::Light& light)
        {
            AppendSnapshot(light);
        });

    PublishLightingDescriptor();
}

void SceneLightingProvider::PrepareRenderScenePackage(
    const NLS::Render::Context::RenderSceneBuildData& scene,
    NLS::Render::Context::RenderScenePackage& package) const
{
    package.hasLightingData = scene.sceneLightCount > 0u;
    package.lightingDataReady = true;
}

const SceneLightingProvider::LightingDescriptor& SceneLightingProvider::GetLightingDescriptor() const
{
    return m_lightingDescriptor;
}

void SceneLightingProvider::PublishLightingDescriptor()
{
    auto snapshot = std::make_shared<LightingDescriptor>(m_lightingDescriptor);
    std::lock_guard lock(m_publishedLightingMutex);
    m_publishedLighting = std::move(snapshot);
}

std::shared_ptr<const SceneLightingProvider::LightingDescriptor>
SceneLightingProvider::ReadPublishedLightingDescriptor() const
{
    std::lock_guard lock(m_publishedLightingMutex);
    return m_publishedLighting;
}
}
