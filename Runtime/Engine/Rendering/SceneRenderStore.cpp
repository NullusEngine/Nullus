#include "Rendering/SceneRenderStore.h"

#include <algorithm>

namespace NLS::Engine::Rendering
{
    namespace
    {
        // Local adaptation for the shared SceneRenderStore: keep removed proxy
        // resources alive until the frames that could still reference them have
        // been retired by the delayed command path.
        constexpr uint64_t kRetiredResourceFrameDelay = 4u;
    }

    SceneRenderStore::SceneRenderStore(
        const uint64_t sceneId,
        const size_t changeJournalCapacity)
        : m_sceneId(sceneId)
        , m_changeJournalCapacity(changeJournalCapacity)
    {
    }

    SceneRenderStore::~SceneRenderStore() = default;

    void SceneRenderStore::QueueRetiredResources(
        const uint64_t frameId,
        const SceneProxyResourceBinding& resources)
    {
        if (resources.meshHandle == nullptr && resources.materialHandle == nullptr)
            return;

        m_retiredResources.push_back({ frameId, resources });
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

    SceneRenderStoreChangeSet SceneRenderStore::GetChangesSince(
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

    void SceneRenderStore::RetireResourcesThrough(const uint64_t frameId)
    {
        if (m_retiredResources.empty())
            return;

        const auto releasedEnd = std::remove_if(
            m_retiredResources.begin(),
            m_retiredResources.end(),
            [frameId](const RetiredResources& retired)
            {
                return retired.frameId + kRetiredResourceFrameDelay <= frameId;
            });
        m_retiredResources.erase(releasedEnd, m_retiredResources.end());
    }

    void SceneRenderStore::Clear()
    {
        for (auto& slot : m_slots)
        {
            if (!slot.occupied)
                continue;
            QueueRetiredResources(m_lastAppliedFrameId, slot.payload.resources);
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
        SceneRenderStoreApplyStats stats;
        if (batch.sceneId != 0u && batch.sceneId != m_sceneId)
            return stats;

        m_lastAppliedFrameId = std::max(m_lastAppliedFrameId, batch.frameId);

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
                    QueueRetiredResources(batch.frameId, slot.payload.resources);
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
                    slot.lightTransform->SetWorldPosition(command.transform.worldPosition);
                    if (slot.payload.light.has_value())
                    {
                        slot.light->type = slot.payload.light->type;
                        slot.light->color = slot.payload.light->color;
                        slot.light->intensity = slot.payload.light->intensity;
                        slot.light->range = slot.payload.light->range;
                        slot.light->cutoff = slot.payload.light->cutoff;
                        slot.light->outerCutoff = slot.payload.light->outerCutoff;
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
                QueueRetiredResources(batch.frameId, slot->payload.resources);
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
                    slot->lightTransform->SetWorldPosition(command.transform.worldPosition);
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
                QueueRetiredResources(batch.frameId, slot->payload.resources);
                slot->payload = command.payload;
                slot->transform = command.transform;
                if (slot->payload.kind == SceneProxyKind::Light)
                {
                    if (slot->lightTransform == nullptr)
                        slot->lightTransform = std::make_unique<NLS::Maths::Transform>();
                    if (slot->light == nullptr)
                        slot->light = std::make_unique<NLS::Render::Entities::Light>(slot->lightTransform.get());
                    slot->lightTransform->SetWorldPosition(command.transform.worldPosition);
                    if (slot->payload.light.has_value())
                    {
                        slot->light->type = slot->payload.light->type;
                        slot->light->color = slot->payload.light->color;
                        slot->light->intensity = slot->payload.light->intensity;
                        slot->light->range = slot->payload.light->range;
                        slot->light->cutoff = slot->payload.light->cutoff;
                        slot->light->outerCutoff = slot->payload.light->outerCutoff;
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

        return stats;
    }

    bool SceneRenderStore::TryGetProxy(
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

    void SceneRenderStore::ForEachProxy(
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

    void SceneRenderStore::ForEachLight(
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

    void SceneRenderStore::ForEachSkyBox(
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
}
