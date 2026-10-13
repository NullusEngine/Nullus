#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include <Rendering/Context/RenderScenePackage.h>
#include <Rendering/Data/LightingDescriptor.h>

#include "EngineDef.h"

namespace NLS::Engine::SceneSystem
{
    class Scene;
}

namespace NLS::Engine::Rendering
{
class SceneRenderStore;

class NLS_ENGINE_API SceneLightingProvider
{
public:
    using LightingDescriptor = NLS::Render::Data::LightingDescriptor;
    using LightSet = NLS::Render::Data::LightSet;

    /**
     * Rebuilds the published light set.
     *
     * The two sources have different ownership, so they publish differently:
     *
     *  - Collect(scene) keeps the live identity of scene component lights. A
     *    component owns its Light for the scene's lifetime, and the published set
     *    is consumed within the same frame, so referencing it is safe and lets
     *    callers correlate a published light with its component.
     *  - CollectFromStore(store) publishes provider-owned snapshots. A store slot
     *    destroys its Light on Remove/Clear, and one store is shared by every
     *    render scene of that Scene, so referencing it would let a removal in one
     *    view dangle the light set another view is still reading.
     *
     * In both cases the published references stay valid until the next Collect
     * call, and the descriptor must not be read from another thread.
     */
    void Collect(const SceneSystem::Scene& scene);
    void CollectFromStore(const SceneRenderStore& store);
    void PrepareRenderScenePackage(
        const NLS::Render::Context::RenderSceneBuildData& scene,
        NLS::Render::Context::RenderScenePackage& package) const;
    const LightingDescriptor& GetLightingDescriptor() const;
    /**
     * Immutable view of the last collection, safe to read from another thread.
     *
     * The live descriptor above is rebuilt in place, so it may only be read by the
     * thread that collects. The published snapshot is a copy taken at the end of
     * each collection: a reader on another thread gets one internally consistent
     * set, and holding it keeps that set valid even after the next collection
     * replaces the live one. The referenced lights stay alive under the same rules
     * the live descriptor relies on - scene-owned for Collect, provider-owned for
     * CollectFromStore.
     */
    [[nodiscard]] std::shared_ptr<const LightingDescriptor> ReadPublishedLightingDescriptor() const;

private:
    /** Resets the published set; the snapshot pool is retained for reuse. */
    void BeginCollection();
    /** Copies one light (and its world transform) into the snapshot pool. */
    void AppendSnapshot(const NLS::Render::Entities::Light& source);
    /** Publishes the finished collection as an immutable snapshot. */
    void PublishLightingDescriptor();

    // Snapshot pool for the store path. Reusing the objects means a steady light
    // count does not allocate every frame, and refs published by an earlier
    // collection stay valid (they observe the refreshed values) instead of
    // dangling. Only the first m_snapshotCount entries are published.
    //
    // shared_ptr rather than unique_ptr keeps the provider copyable, which was its
    // previous behaviour, and a copied provider aliases the same published lights
    // exactly as it used to alias the source lights.
    std::vector<std::shared_ptr<NLS::Maths::Transform>> m_snapshotTransforms;
    std::vector<std::shared_ptr<NLS::Render::Entities::Light>> m_snapshotLights;
    size_t m_snapshotCount = 0u;
    LightingDescriptor m_lightingDescriptor;
    mutable std::mutex m_publishedLightingMutex;
    std::shared_ptr<const LightingDescriptor> m_publishedLighting;
};
}
