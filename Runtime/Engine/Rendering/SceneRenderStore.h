#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "Rendering/SceneProxy.h"
#include "Rendering/PersistentObjectDataTable.h"
#include <Rendering/Entities/Light.h>
#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
    struct SceneRenderStoreApplyStats
    {
        uint64_t addedProxyCount = 0u;
        uint64_t updatedTransformCount = 0u;
        uint64_t updatedPayloadCount = 0u;
        uint64_t removedProxyCount = 0u;
        uint64_t staleCommandCount = 0u;
    };

    struct SceneRenderStoreChangeSet
    {
        uint64_t changeSerial = 0u;
        bool requiresFullResync = false;
        std::vector<SceneProxyHandle> changedHandles;
        std::vector<SceneProxyHandle> removedHandles;
    };

    /**
     * Render-side persistent proxy store. It is the local FScene-like owner for
     * proxy state and must only be mutated on the Render Thread or by the
     * inline graph executor.
     */
    class NLS_ENGINE_API SceneRenderStore final
    {
    public:
        explicit SceneRenderStore(
            uint64_t sceneId = 0u,
            size_t changeJournalCapacity = 4096u);
        ~SceneRenderStore();

        SceneRenderStore(const SceneRenderStore&) = delete;
        SceneRenderStore& operator=(const SceneRenderStore&) = delete;

        SceneRenderStoreApplyStats Apply(const SceneProxyUpdateBatch& batch);
        void Clear();

        /**
         * Releases proxy resources whose owning frame has been retired for longer
         * than the local in-flight window. Removed proxies keep their ResourceHandle
         * bindings alive until this runs so delayed render commands cannot observe
         * freed mesh/material resources.
         */
        void RetireResourcesThrough(uint64_t frameId);
        [[nodiscard]] size_t GetPendingResourceRetireCount() const noexcept
        {
            return m_retiredResources.size();
        }

        [[nodiscard]] uint64_t GetSceneId() const noexcept { return m_sceneId; }
        [[nodiscard]] size_t GetProxyCount() const noexcept { return m_liveProxyCount; }
        [[nodiscard]] uint64_t GetChangeSerial() const noexcept { return m_changeSerial; }
        [[nodiscard]] SceneRenderStoreChangeSet GetChangesSince(uint64_t changeSerial) const;
        [[nodiscard]] bool TryGetProxy(
            SceneProxyHandle handle,
            const SceneProxyPayload*& outPayload,
            const SceneProxyTransformState*& outTransform) const noexcept;

        void ForEachProxy(
            const std::function<void(
                const SceneProxyHandle&,
                const SceneProxyPayload&,
                const SceneProxyTransformState&)>& visitor) const;

        void ForEachLight(
            const std::function<void(const NLS::Render::Entities::Light&)>& visitor) const;
        void ForEachSkyBox(
            const std::function<void(
                const SceneProxyHandle&,
                const SceneProxyPayload&,
                const SceneProxyTransformState&)>& visitor) const;

        [[nodiscard]] const PersistentObjectDataTable& GetObjectDataTable() const noexcept
        {
            return m_objectDataTable;
        }

    private:
        struct Slot final
        {
            SceneProxyHandle handle;
            SceneProxyPayload payload;
            SceneProxyTransformState transform;
            std::unique_ptr<NLS::Maths::Transform> lightTransform;
            std::unique_ptr<NLS::Render::Entities::Light> light;
            bool occupied = false;
        };

        [[nodiscard]] bool ValidateHandle(SceneProxyHandle handle) const noexcept;
        Slot* FindSlot(SceneProxyHandle handle) noexcept;
        const Slot* FindSlot(SceneProxyHandle handle) const noexcept;
        void QueueRetiredResources(uint64_t frameId, const SceneProxyResourceBinding& resources);
        void RecordChange(SceneProxyHandle handle, bool removed);

        struct ChangeRecord final
        {
            uint64_t serial = 0u;
            SceneProxyHandle handle;
            bool removed = false;
        };

        struct RetiredResources final
        {
            uint64_t frameId = 0u;
            SceneProxyResourceBinding resources;
        };

        uint64_t m_sceneId = 0u;
        std::vector<Slot> m_slots;
        size_t m_liveProxyCount = 0u;
        PersistentObjectDataTable m_objectDataTable;
        std::vector<RetiredResources> m_retiredResources;
        uint64_t m_lastAppliedFrameId = 0u;
        size_t m_changeJournalCapacity = 0u;
        std::deque<ChangeRecord> m_changeJournal;
        uint64_t m_changeSerial = 0u;
        uint64_t m_minAvailableChangeSerial = 0u;
        uint64_t m_lastInvalidatingChangeSerial = 0u;
    };
}
