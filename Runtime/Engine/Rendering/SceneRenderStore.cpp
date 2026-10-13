#include "Rendering/SceneRenderStore.h"

#include <algorithm>
#include <stdexcept>
#include <Rendering/Context/RenderingThreadSystem.h>
#include <Rendering/Context/RhiCommandListExecutor.h>

namespace NLS::Engine::Rendering
{
    SceneRenderStore::SceneRenderStore(
        const uint64_t sceneId,
        const size_t changeJournalCapacity)
        : m_sceneId(sceneId)
        , m_changeJournalCapacity(changeJournalCapacity)
    {
    }

    SceneRenderStore::~SceneRenderStore()
    {
        // Last-owner destruction also occurs when a failed pipe drops closures.
        // No caller can access this object now; the thread-safe executor retains
        // its bindings even when GPU completion cannot be proved.
        ClearInternal();
    }

    void SceneRenderStore::RequireRenderThread()
    {
        if (!NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread())
            throw std::logic_error("SceneRenderStore contents require the render thread");
    }

    void SceneRenderStore::RequireMutation() const
    {
        RequireRenderThread();
        if (m_readViewCount != 0u)
            throw std::logic_error("SceneRenderStore mutation cannot invalidate an active read view");
    }

    void SceneRenderStore::PublishMetadata() noexcept
    {
        m_publishedProxyCount.store(m_liveProxyCount, std::memory_order_release);
        m_publishedChangeSerial.store(m_changeSerial, std::memory_order_release);
    }

    void SceneRenderStore::QueueRetiredResources(
        const SceneProxyResourceBinding& resources)
    {
        if (resources.meshHandle == nullptr && resources.materialHandle == nullptr)
            return;

        m_retiredResources.push_back({ resources, std::nullopt });
    }

    void SceneRenderStore::RecordChange(
        const SceneProxyHandle handle,
        const bool removed)
    {
        ++m_changeSerial;
        if (m_changeSerial == 0u)
        {
            m_changeSerial = 1u;
            m_changeJournal.clear();
            m_minAvailableChangeSerial = m_changeSerial;
            m_lastInvalidatingChangeSerial = m_changeSerial;
        }

        if (m_changeJournalCapacity == 0u)
        {
            m_changeJournal.clear();
            m_minAvailableChangeSerial = m_changeSerial;
            return;
        }

        m_changeJournal.push_back({ m_changeSerial, handle, removed });
        while (m_changeJournal.size() > m_changeJournalCapacity)
        {
            m_minAvailableChangeSerial = std::max(
                m_minAvailableChangeSerial,
                m_changeJournal.front().serial);
            m_changeJournal.pop_front();
        }
    }

    SceneRenderStoreChangeSet SceneRenderStore::GetChangesSinceInternal(
        const uint64_t changeSerial) const
    {
        SceneRenderStoreChangeSet result;
        result.changeSerial = m_changeSerial;
        if (changeSerial == m_changeSerial)
            return result;

        if (changeSerial > m_changeSerial ||
            changeSerial < m_minAvailableChangeSerial ||
            changeSerial < m_lastInvalidatingChangeSerial)
        {
            result.requiresFullResync = true;
            return result;
        }

        for (const auto& change : m_changeJournal)
        {
            if (change.serial <= changeSerial)
                continue;
            if (change.removed)
                result.removedHandles.push_back(change.handle);
            else
                result.changedHandles.push_back(change.handle);
        }
        return result;
    }

    void SceneRenderStore::RetireResources()
    {
        RequireMutation();
        RetireResourcesInternal();
    }

    void SceneRenderStore::RetireResourcesInternal()
    {
        if (m_retiredResources.empty())
            return;

        std::vector<std::shared_ptr<void>> owners;
        for (const auto& retired : m_retiredResources)
        {
            if (retired.gpuFence.has_value())
                continue;
            if (retired.resources.meshHandle != nullptr)
                owners.push_back(retired.resources.meshHandle);
            if (retired.resources.materialHandle != nullptr)
                owners.push_back(retired.resources.materialHandle);
        }
        if (!owners.empty())
        {
            const auto gpuFence = NLS::Render::Context::RhiCommandListExecutor::Get().DeferRelease(std::move(owners));
            for (auto& retired : m_retiredResources)
            {
                if (retired.gpuFence.has_value())
                    continue;
                retired.gpuFence = gpuFence;
                retired.resources = {};
            }
        }
        const auto releasedEnd = std::remove_if(
            m_retiredResources.begin(),
            m_retiredResources.end(),
            [](const RetiredResources& retired)
            {
                return retired.gpuFence.has_value() && retired.gpuFence->Succeeded();
            });
        m_retiredResources.erase(releasedEnd, m_retiredResources.end());
    }

    size_t SceneRenderStore::GetPendingResourceRetireCount() const
    {
        RequireRenderThread();
        return static_cast<size_t>(std::count_if(m_retiredResources.begin(), m_retiredResources.end(),
            [](const auto& retired) { return !retired.gpuFence.has_value() || !retired.gpuFence->Succeeded(); }));
    }

    void SceneRenderStore::Clear()
    {
        RequireMutation();
        ClearInternal();
    }

    void SceneRenderStore::ClearInternal()
    {
        for (auto& slot : m_slots)
        {
            if (!slot.occupied)
                continue;
            QueueRetiredResources(slot.payload.resources);
            slot.occupied = false;
            ++slot.handle.generation;
            if (slot.handle.generation == 0u)
                slot.handle.generation = 1u;
            slot.payload = {};
            slot.transform = {};
            slot.lightTransform.reset();
            slot.light.reset();
        }
        m_liveProxyCount = 0u;
        m_objectDataTable.Clear();
        ++m_changeSerial;
        if (m_changeSerial == 0u)
            m_changeSerial = 1u;
        m_changeJournal.clear();
        m_minAvailableChangeSerial = m_changeSerial;
        m_lastInvalidatingChangeSerial = m_changeSerial;
        PublishMetadata();
        RetireResourcesInternal();
    }

    bool SceneRenderStore::ValidateHandle(const SceneProxyHandle handle) const noexcept
    {
        if (!handle.IsValid() || handle.sceneId != m_sceneId || handle.index >= m_slots.size())
            return false;
        const auto& slot = m_slots[handle.index];
        return slot.occupied && slot.handle.generation == handle.generation;
    }

    SceneRenderStore::Slot* SceneRenderStore::FindSlot(const SceneProxyHandle handle) noexcept
    {
        if (!ValidateHandle(handle))
            return nullptr;
        return &m_slots[handle.index];
    }

    const SceneRenderStore::Slot* SceneRenderStore::FindSlot(const SceneProxyHandle handle) const noexcept
    {
        if (!ValidateHandle(handle))
            return nullptr;
        return &m_slots[handle.index];
    }

    SceneRenderStoreApplyStats SceneRenderStore::Apply(const SceneProxyUpdateBatch& batch)
    {
        RequireMutation();

        SceneRenderStoreApplyStats stats;
        if (batch.sceneId != 0u && batch.sceneId != m_sceneId)
            return stats;

        for (const auto& command : batch.commands)
        {
            if (!command.handle.IsValid() || command.handle.sceneId != m_sceneId)
            {
                ++stats.staleCommandCount;
                continue;
            }

            if (command.kind == SceneProxyCommandKind::Add)
            {
                if (command.handle.index >= m_slots.size())
                    m_slots.resize(static_cast<size_t>(command.handle.index) + 1u);

                auto& slot = m_slots[command.handle.index];
                const bool replacingOccupiedSlot = slot.occupied;
                const auto replacedHandle = slot.handle;
                if (replacingOccupiedSlot)
                {
                    QueueRetiredResources(slot.payload.resources);
                    m_objectDataTable.Release(slot.handle);
                    if (!(replacedHandle == command.handle))
                        RecordChange(replacedHandle, true);
                }
                slot.handle = command.handle;
                slot.payload = command.payload;
                slot.transform = command.transform;
                slot.occupied = true;
                if (slot.payload.kind == SceneProxyKind::Light)
                {
                    if (slot.lightTransform == nullptr)
                        slot.lightTransform = std::make_unique<NLS::Maths::Transform>();
                    if (slot.light == nullptr)
                        slot.light = std::make_unique<NLS::Render::Entities::Light>(slot.lightTransform.get());
                    slot.lightTransform->GenerateMatricesWorld(
                        command.transform.worldPosition,
                        command.transform.worldRotation,
                        command.transform.worldScale);
                    if (slot.payload.light.has_value())
                    {
                        slot.light->type = slot.payload.light->type;
                        slot.light->color = slot.payload.light->color;
                        slot.light->intensity = slot.payload.light->intensity;
                        slot.light->range = slot.payload.light->range;
                        slot.light->cutoff = slot.payload.light->cutoff;
                        slot.light->outerCutoff = slot.payload.light->outerCutoff;
                        slot.light->size = slot.payload.light->size;
                    }
                }
                if (slot.payload.kind != SceneProxyKind::Light)
                {
                    m_objectDataTable.Acquire(
                        command.handle,
                        command.transform.worldMatrix,
                        command.transform.transformRevision);
                }
                if (replacingOccupiedSlot)
                    ++stats.updatedPayloadCount;
                else
                {
                    ++stats.addedProxyCount;
                    ++m_liveProxyCount;
                }
                RecordChange(command.handle, false);
                continue;
            }

            auto* slot = FindSlot(command.handle);
            if (slot == nullptr)
            {
                ++stats.staleCommandCount;
                continue;
            }

            switch (command.kind)
            {
            case SceneProxyCommandKind::Remove:
                QueueRetiredResources(slot->payload.resources);
                RecordChange(command.handle, true);
                slot->occupied = false;
                ++slot->handle.generation;
                if (slot->handle.generation == 0u)
                    slot->handle.generation = 1u;
                slot->payload = {};
                slot->transform = {};
                slot->lightTransform.reset();
                slot->light.reset();
                m_objectDataTable.Release(command.handle);
                --m_liveProxyCount;
                ++stats.removedProxyCount;
                break;
            case SceneProxyCommandKind::UpdateTransform:
                slot->transform = command.transform;
                if (slot->lightTransform != nullptr)
                    slot->lightTransform->GenerateMatricesWorld(
                        command.transform.worldPosition,
                        command.transform.worldRotation,
                        command.transform.worldScale);
                if (slot->payload.kind != SceneProxyKind::Light)
                {
                    (void)m_objectDataTable.Update(
                        command.handle,
                        command.transform.worldMatrix,
                        command.transform.transformRevision);
                }
                ++stats.updatedTransformCount;
                RecordChange(command.handle, false);
                break;
            case SceneProxyCommandKind::UpdatePayload:
            case SceneProxyCommandKind::Recreate:
                QueueRetiredResources(slot->payload.resources);
                slot->payload = command.payload;
                slot->transform = command.transform;
                if (slot->payload.kind == SceneProxyKind::Light)
                {
                    if (slot->lightTransform == nullptr)
                        slot->lightTransform = std::make_unique<NLS::Maths::Transform>();
                    if (slot->light == nullptr)
                        slot->light = std::make_unique<NLS::Render::Entities::Light>(slot->lightTransform.get());
                    slot->lightTransform->GenerateMatricesWorld(
                        command.transform.worldPosition,
                        command.transform.worldRotation,
                        command.transform.worldScale);
                    if (slot->payload.light.has_value())
                    {
                        slot->light->type = slot->payload.light->type;
                        slot->light->color = slot->payload.light->color;
                        slot->light->intensity = slot->payload.light->intensity;
                        slot->light->range = slot->payload.light->range;
                        slot->light->cutoff = slot->payload.light->cutoff;
                        slot->light->outerCutoff = slot->payload.light->outerCutoff;
                        slot->light->size = slot->payload.light->size;
                    }
                }
                else
                {
                    slot->lightTransform.reset();
                    slot->light.reset();
                }
                if (slot->payload.kind != SceneProxyKind::Light)
                {
                    (void)m_objectDataTable.Acquire(
                        command.handle,
                        command.transform.worldMatrix,
                        command.transform.transformRevision);
                    (void)m_objectDataTable.Update(
                        command.handle,
                        command.transform.worldMatrix,
                        command.transform.transformRevision);
                }
                else
                {
                    m_objectDataTable.Release(command.handle);
                }
                ++stats.updatedPayloadCount;
                RecordChange(command.handle, false);
                break;
            case SceneProxyCommandKind::Add:
            default:
                ++stats.staleCommandCount;
                break;
            }
        }

        PublishMetadata();
        RetireResources();
        return stats;
    }

    bool SceneRenderStore::TryGetProxyInternal(
        const SceneProxyHandle handle,
        const SceneProxyPayload*& outPayload,
        const SceneProxyTransformState*& outTransform) const noexcept
    {
        const auto* slot = FindSlot(handle);
        if (slot == nullptr)
        {
            outPayload = nullptr;
            outTransform = nullptr;
            return false;
        }

        outPayload = &slot->payload;
        outTransform = &slot->transform;
        return true;
    }

    void SceneRenderStore::ForEachProxyInternal(
        const std::function<void(
            const SceneProxyHandle&,
            const SceneProxyPayload&,
            const SceneProxyTransformState&)>& visitor) const
    {
        if (!visitor)
            return;

        for (const auto& slot : m_slots)
        {
            if (slot.occupied)
                visitor(slot.handle, slot.payload, slot.transform);
        }
    }

    void SceneRenderStore::ForEachLightInternal(
        const std::function<void(const NLS::Render::Entities::Light&)>& visitor) const
    {
        if (!visitor)
            return;
        for (const auto& slot : m_slots)
        {
            if (slot.occupied && slot.light != nullptr)
                visitor(*slot.light);
        }
    }

    void SceneRenderStore::ForEachSkyBoxInternal(
        const std::function<void(
            const SceneProxyHandle&,
            const SceneProxyPayload&,
            const SceneProxyTransformState&)>& visitor) const
    {
        if (!visitor)
            return;
        for (const auto& slot : m_slots)
        {
            if (slot.occupied && slot.payload.kind == SceneProxyKind::SkyBox)
                visitor(slot.handle, slot.payload, slot.transform);
        }
    }

    size_t SceneRenderStore::GetProxyCount() const noexcept
    {
        return m_publishedProxyCount.load(std::memory_order_acquire);
    }

    uint64_t SceneRenderStore::GetChangeSerial() const noexcept
    {
        return m_publishedChangeSerial.load(std::memory_order_acquire);
    }

    SceneRenderStoreChangeSet SceneRenderStore::GetChangesSince(const uint64_t changeSerial) const
    {
        RequireRenderThread();
        return GetChangesSinceInternal(changeSerial);
    }

    bool SceneRenderStore::TryGetProxy(
        const SceneProxyHandle handle,
        const SceneProxyPayload*& outPayload,
        const SceneProxyTransformState*& outTransform) const
    {
        RequireRenderThread();
        return TryGetProxyInternal(handle, outPayload, outTransform);
    }

    void SceneRenderStore::ForEachProxy(
        const std::function<void(
            const SceneProxyHandle&,
            const SceneProxyPayload&,
            const SceneProxyTransformState&)>& visitor) const
    {
        const auto view = Read();
        view.ForEachProxy(visitor);
    }

    void SceneRenderStore::ForEachLight(
        const std::function<void(const NLS::Render::Entities::Light&)>& visitor) const
    {
        const auto view = Read();
        view.ForEachLight(visitor);
    }

    void SceneRenderStore::ForEachSkyBox(
        const std::function<void(
            const SceneProxyHandle&,
            const SceneProxyPayload&,
            const SceneProxyTransformState&)>& visitor) const
    {
        const auto view = Read();
        view.ForEachSkyBox(visitor);
    }

    const PersistentObjectDataTable& SceneRenderStore::GetObjectDataTable() const
    {
        RequireRenderThread();
        return m_objectDataTable;
    }

    SceneRenderStore::ReadView SceneRenderStore::Read() const
    {
        return ReadView(*this);
    }

    SceneRenderStore::ReadView::ReadView(const SceneRenderStore& store)
        : m_store(store)
    {
        RequireRenderThread();
        ++m_store.m_readViewCount;
    }

    SceneRenderStore::ReadView::~ReadView()
    {
        --m_store.m_readViewCount;
    }

    size_t SceneRenderStore::ReadView::GetProxyCount() const
    {
        RequireRenderThread();
        return m_store.m_liveProxyCount;
    }

    uint64_t SceneRenderStore::ReadView::GetChangeSerial() const
    {
        RequireRenderThread();
        return m_store.m_changeSerial;
    }

    SceneRenderStoreChangeSet SceneRenderStore::ReadView::GetChangesSince(
        const uint64_t changeSerial) const
    {
        RequireRenderThread();
        return m_store.GetChangesSinceInternal(changeSerial);
    }

    bool SceneRenderStore::ReadView::TryGetProxy(
        const SceneProxyHandle handle,
        const SceneProxyPayload*& outPayload,
        const SceneProxyTransformState*& outTransform) const
    {
        RequireRenderThread();
        return m_store.TryGetProxyInternal(handle, outPayload, outTransform);
    }

    void SceneRenderStore::ReadView::ForEachProxy(
        const std::function<void(
            const SceneProxyHandle&,
            const SceneProxyPayload&,
            const SceneProxyTransformState&)>& visitor) const
    {
        RequireRenderThread();
        m_store.ForEachProxyInternal(visitor);
    }

    void SceneRenderStore::ReadView::ForEachLight(
        const std::function<void(const NLS::Render::Entities::Light&)>& visitor) const
    {
        RequireRenderThread();
        m_store.ForEachLightInternal(visitor);
    }

    void SceneRenderStore::ReadView::ForEachSkyBox(
        const std::function<void(
            const SceneProxyHandle&,
            const SceneProxyPayload&,
            const SceneProxyTransformState&)>& visitor) const
    {
        RequireRenderThread();
        m_store.ForEachSkyBoxInternal(visitor);
    }

    const PersistentObjectDataTable& SceneRenderStore::ReadView::GetObjectDataTable() const
    {
        RequireRenderThread();
        return m_store.m_objectDataTable;
    }
}
