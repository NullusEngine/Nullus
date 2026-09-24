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
     * Process-wide weak registry that gives every view renderer the same
     * persistent proxy store and game-thread source registry for a Scene.
     */
    class NLS_ENGINE_API SceneRenderStoreRegistry final
    {
    public:
        static SceneRenderStoreEntry Acquire(uint64_t sceneId, const void* sceneIdentity);
        static void Release(const void* sceneIdentity);
        static void Trim();

    private:
        SceneRenderStoreRegistry() = delete;
    };
}
