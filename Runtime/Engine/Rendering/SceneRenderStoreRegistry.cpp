#include "Rendering/SceneRenderStoreRegistry.h"

#include "Rendering/SceneProxySourceRegistry.h"
#include "Rendering/SceneRenderStore.h"

#include <mutex>
#include <unordered_map>

namespace NLS::Engine::Rendering
{
    namespace
    {
        struct EntryState final
        {
            explicit EntryState(const uint64_t sceneId)
                : sourceRegistry(std::make_shared<SceneProxySourceRegistry>())
                , store(std::make_shared<SceneRenderStore>(sceneId))
            {
            }

            std::shared_ptr<SceneProxySourceRegistry> sourceRegistry;
            std::shared_ptr<SceneRenderStore> store;
        };

        std::mutex g_registryMutex;
        std::unordered_map<const void*, std::shared_ptr<EntryState>> g_entries;
    }

    SceneRenderStoreEntry SceneRenderStoreRegistry::Acquire(
        const uint64_t sceneId,
        const void* sceneIdentity)
    {
        if (sceneId == 0u || sceneIdentity == nullptr)
            return {};

        std::lock_guard lock(g_registryMutex);
        auto& entry = g_entries[sceneIdentity];
        if (entry == nullptr || entry->store->GetSceneId() != sceneId)
            entry = std::make_shared<EntryState>(sceneId);

        return SceneRenderStoreEntry { entry->sourceRegistry, entry->store };
    }

    void SceneRenderStoreRegistry::Release(const void* sceneIdentity)
    {
        if (sceneIdentity == nullptr)
            return;
        std::lock_guard lock(g_registryMutex);
        g_entries.erase(sceneIdentity);
    }

    void SceneRenderStoreRegistry::Trim()
    {
        std::lock_guard lock(g_registryMutex);
        for (auto iterator = g_entries.begin(); iterator != g_entries.end();)
        {
            if (iterator->second == nullptr ||
                (iterator->second->sourceRegistry.use_count() == 1 &&
                 iterator->second->store.use_count() == 1))
            {
                iterator = g_entries.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
    }
}
