#pragma once

#include <cstdint>
#include <memory>

#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
    class SceneProxySourceRegistry;
    class SceneRenderStore;

    struct SceneRenderStoreEntry
    {
        std::shared_ptr<SceneProxySourceRegistry> sourceRegistry;
        std::shared_ptr<SceneRenderStore> store;
    };

    /**
     * Scene-owned registry that gives every view renderer the same persistent
     * proxy store and game-thread source registry until the Scene is destroyed.
     */
    class NLS_ENGINE_API SceneRenderStoreRegistry final
    {
    public:
        static SceneRenderStoreEntry Acquire(uint64_t sceneId, const void* sceneIdentity);
        static void Release(const void* sceneIdentity);
        /** Call after frame producers stop, before the render lane/resource managers shut down. */
        static void ReleaseAll();
        static void Trim();

    private:
        SceneRenderStoreRegistry() = delete;
        static void RetireStore(std::shared_ptr<SceneRenderStore> store);
    };
}
