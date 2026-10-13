#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <atomic>
#include <vector>

#include "Rendering/SceneProxy.h"
#include "Rendering/PersistentObjectDataTable.h"
#include <Rendering/Entities/Light.h>
#include <Rendering/Context/RhiCommandList.h>
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
     * proxy state.
     *
     * Proxy contents, change journals and object data belong to the render thread.
     * Game/worker threads may only read the published scalar count and serial.
     * Workers prepare immutable inputs copied by the render thread. Read views
     * reject reentrant mutation while their slot references are in use.
     */
    class NLS_ENGINE_API SceneRenderStore final
    {
    public:
        /**
         * Scoped render-thread access.
         *
         * Hold this for the whole time slot pointers or references from the store
         * are in use. Pointers obtained through a view stay valid until the view
         * is destroyed; pointers obtained from the point-in-time convenience
         * methods on the store itself are only valid until the next mutation.
         */
        class NLS_ENGINE_API ReadView final
        {
        public:
            ReadView(const ReadView&) = delete;
            ReadView& operator=(const ReadView&) = delete;
            ReadView(ReadView&&) = delete;
            ReadView& operator=(ReadView&&) = delete;
            ~ReadView();

            [[nodiscard]] size_t GetProxyCount() const;
            [[nodiscard]] uint64_t GetChangeSerial() const;
            [[nodiscard]] SceneRenderStoreChangeSet GetChangesSince(uint64_t changeSerial) const;
            [[nodiscard]] bool TryGetProxy(
                SceneProxyHandle handle,
                const SceneProxyPayload*& outPayload,
                const SceneProxyTransformState*& outTransform) const;
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
            [[nodiscard]] const PersistentObjectDataTable& GetObjectDataTable() const;

        private:
            friend class SceneRenderStore;
            explicit ReadView(const SceneRenderStore& store);

            const SceneRenderStore& m_store;
        };

        /** Keeps render-thread slot references valid until the view is destroyed. */
        [[nodiscard]] ReadView Read() const;

        explicit SceneRenderStore(
            uint64_t sceneId = 0u,
            size_t changeJournalCapacity = 4096u);
        ~SceneRenderStore();

        SceneRenderStore(const SceneRenderStore&) = delete;
        SceneRenderStore& operator=(const SceneRenderStore&) = delete;

        SceneRenderStoreApplyStats Apply(const SceneProxyUpdateBatch& batch);
        void Clear();

        /** Transfers removed bindings to the executor until every earlier GPU queue retires. */
        void RetireResources();
        [[nodiscard]] size_t GetPendingResourceRetireCount() const;

        [[nodiscard]] uint64_t GetSceneId() const noexcept { return m_sceneId; }
        /** Published metadata only; these do not expose proxy contents. */
        [[nodiscard]] size_t GetProxyCount() const noexcept;
        [[nodiscard]] uint64_t GetChangeSerial() const noexcept;
        [[nodiscard]] SceneRenderStoreChangeSet GetChangesSince(uint64_t changeSerial) const;
        [[nodiscard]] bool TryGetProxy(
            SceneProxyHandle handle,
            const SceneProxyPayload*& outPayload,
            const SceneProxyTransformState*& outTransform) const;

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

        [[nodiscard]] const PersistentObjectDataTable& GetObjectDataTable() const;

    private:
        friend class SceneRenderStoreRegistry;
        static void RequireRenderThread();
        void RequireMutation() const;
        void PublishMetadata() noexcept;
        void ClearInternal();
        void RetireResourcesInternal();
        [[nodiscard]] SceneRenderStoreChangeSet GetChangesSinceInternal(uint64_t changeSerial) const;
        [[nodiscard]] bool TryGetProxyInternal(
            SceneProxyHandle handle,
            const SceneProxyPayload*& outPayload,
            const SceneProxyTransformState*& outTransform) const noexcept;
        void ForEachProxyInternal(
            const std::function<void(
                const SceneProxyHandle&,
                const SceneProxyPayload&,
                const SceneProxyTransformState&)>& visitor) const;
        void ForEachLightInternal(
            const std::function<void(const NLS::Render::Entities::Light&)>& visitor) const;
        void ForEachSkyBoxInternal(
            const std::function<void(
                const SceneProxyHandle&,
                const SceneProxyPayload&,
                const SceneProxyTransformState&)>& visitor) const;

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
        void QueueRetiredResources(const SceneProxyResourceBinding& resources);
        void RecordChange(SceneProxyHandle handle, bool removed);

        struct ChangeRecord final
        {
            uint64_t serial = 0u;
            SceneProxyHandle handle;
            bool removed = false;
        };

        struct RetiredResources final
        {
            SceneProxyResourceBinding resources;
            std::optional<NLS::Render::Context::RhiGpuFence> gpuFence;
        };

        uint64_t m_sceneId = 0u;
        std::deque<Slot> m_slots;
        mutable size_t m_readViewCount = 0u;
        size_t m_liveProxyCount = 0u;
        std::atomic<size_t> m_publishedProxyCount { 0u };
        std::atomic<uint64_t> m_publishedChangeSerial { 0u };
        PersistentObjectDataTable m_objectDataTable;
        std::vector<RetiredResources> m_retiredResources;
        size_t m_changeJournalCapacity = 0u;
        std::deque<ChangeRecord> m_changeJournal;
        uint64_t m_changeSerial = 0u;
        uint64_t m_minAvailableChangeSerial = 0u;
        uint64_t m_lastInvalidatingChangeSerial = 0u;
    };
}
