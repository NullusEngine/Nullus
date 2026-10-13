#include "Rendering/SceneRenderStoreRegistry.h"

#include "Rendering/SceneProxySourceRegistry.h"
#include "Rendering/SceneRenderStore.h"
#include <Rendering/Context/RenderCommandPipe.h>
#include <Rendering/Context/RenderingThreadSystem.h>

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
                , store(new SceneRenderStore(sceneId), [](SceneRenderStore* retired)
                  {
                      if (NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread() ||
                          !NLS::Render::Context::RenderingThreadSystem::HasActiveRenderThread())
                      {
                          delete retired;
                          return;
                      }
                      // A last game-side handle may disappear ahead of queued
                      // frames. Keep destruction ordered with those frames.
                      NLS::Render::Context::EnqueueRenderCommand<struct ReleaseSceneRenderStoreCommand>(
                          [owner = std::shared_ptr<SceneRenderStore>(retired)] {});
                  })
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
        std::shared_ptr<EntryState> retired;
        {
            std::lock_guard lock(g_registryMutex);
            const auto entry = g_entries.find(sceneIdentity);
            if (entry == g_entries.end())
                return;
            retired = std::move(entry->second);
            g_entries.erase(entry);
        }
        if (retired != nullptr)
            RetireStore(std::move(retired->store));
    }

    void SceneRenderStoreRegistry::ReleaseAll()
    {
        decltype(g_entries) retired;
        {
            std::lock_guard lock(g_registryMutex);
            retired.swap(g_entries);
        }
        for (auto& [identity, entry] : retired)
            if (entry != nullptr)
                RetireStore(std::move(entry->store));
    }

    void SceneRenderStoreRegistry::RetireStore(std::shared_ptr<SceneRenderStore> store)
    {
        if (store == nullptr)
            return;
        if (NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread())
        {
            store->Clear();
        }
        else if (NLS::Render::Context::RenderingThreadSystem::HasActiveRenderThread())
        {
            // Cached RenderScene owners can survive the Scene. Clear their leases
            // after earlier frame builds, even when those owners keep the store alive.
            NLS::Render::Context::EnqueueRenderCommand<struct ClearSceneRenderStoreCommand>(
                [store = std::move(store)] { store->Clear(); });
        }
        else
        {
            // The lane is joined; the executor still fences or quarantines GPU owners.
            store->ClearInternal();
        }
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
