#pragma once

#include <cstdint>
#include <memory>

#include "Rendering/SceneProxy.h"
#include "EngineDef.h"

namespace NLS::Engine::SceneSystem
{
    class Scene;
}

namespace NLS::Engine::Rendering
{
    /**
     * Game-thread owned source registry. It observes scene revisions and emits
     * only add/update/remove commands; the render side never dereferences a
     * GameObject or Component pointer from this registry.
     */
    class NLS_ENGINE_API SceneProxySourceRegistry final
    {
    public:
        SceneProxySourceRegistry();
        ~SceneProxySourceRegistry();

        SceneProxySourceRegistry(const SceneProxySourceRegistry&) = delete;
        SceneProxySourceRegistry& operator=(const SceneProxySourceRegistry&) = delete;

        SceneProxyUpdateBatch Collect(
            SceneSystem::Scene& scene,
            const SceneProxyExtractionOptions& options,
            uint64_t frameId = 0u);

        void Reset();

        [[nodiscard]] size_t GetTrackedPrimitiveProxyCount() const;
        [[nodiscard]] size_t GetTrackedLightProxyCount() const;
        [[nodiscard]] size_t GetTrackedSkyBoxProxyCount() const;
        [[nodiscard]] uint64_t GetCollectionScanCountForTesting() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
