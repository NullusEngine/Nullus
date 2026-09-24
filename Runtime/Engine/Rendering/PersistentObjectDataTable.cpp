#include "Rendering/PersistentObjectDataTable.h"

#include <algorithm>
#include <cstring>

namespace NLS::Engine::Rendering
{
    namespace
    {
        uint64_t MakeProxyKey(const SceneProxyHandle handle)
        {
            return (static_cast<uint64_t>(handle.index) << 32u) |
                static_cast<uint64_t>(handle.generation);
        }
    }

    uint32_t PersistentObjectDataTable::Acquire(
        const SceneProxyHandle handle,
        const Maths::Matrix4& modelMatrix,
        const uint64_t revision)
    {
        if (!handle.IsValid())
            return ~0u;

        const auto existing = m_slotByProxyId.find(MakeProxyKey(handle));
        if (existing != m_slotByProxyId.end())
            return existing->second;

        uint32_t slotIndex = ~0u;
        if (!m_freeSlots.empty())
        {
            slotIndex = m_freeSlots.back();
            m_freeSlots.pop_back();
        }
        else
        {
            slotIndex = static_cast<uint32_t>(m_slots.size());
            m_slots.emplace_back();
        }

        auto& slot = m_slots[slotIndex];
        slot.handle = handle;
        slot.modelMatrix = modelMatrix;
        slot.revision = revision;
        slot.occupied = true;
        slot.dirty = true;
        m_capacity = (std::max)(m_capacity, slotIndex + 1u);
        m_slotByProxyId.emplace(MakeProxyKey(handle), slotIndex);
        return slotIndex;
    }

    void PersistentObjectDataTable::Release(const SceneProxyHandle handle)
    {
        if (!handle.IsValid())
            return;

        const auto found = m_slotByProxyId.find(MakeProxyKey(handle));
        if (found == m_slotByProxyId.end())
            return;

        auto& slot = m_slots[found->second];
        slot.occupied = false;
        slot.dirty = false;
        m_freeSlots.push_back(found->second);
        m_slotByProxyId.erase(found);
    }

    bool PersistentObjectDataTable::Update(
        const SceneProxyHandle handle,
        const Maths::Matrix4& modelMatrix,
        const uint64_t revision)
    {
        if (!handle.IsValid())
            return false;

        const auto found = m_slotByProxyId.find(MakeProxyKey(handle));
        if (found == m_slotByProxyId.end())
            return false;

        auto& slot = m_slots[found->second];
        if (slot.revision == revision && std::memcmp(&slot.modelMatrix, &modelMatrix, sizeof(modelMatrix)) == 0)
            return false;

        slot.modelMatrix = modelMatrix;
        slot.revision = revision;
        slot.dirty = true;
        return true;
    }

    bool PersistentObjectDataTable::TryGetSlot(
        const SceneProxyHandle handle,
        uint32_t& outSlot) const
    {
        const auto found = m_slotByProxyId.find(MakeProxyKey(handle));
        if (found == m_slotByProxyId.end())
            return false;
        outSlot = found->second;
        return true;
    }

    std::vector<uint32_t> PersistentObjectDataTable::BuildInstanceIndirection(
        const std::vector<SceneProxyHandle>& handles) const
    {
        std::vector<uint32_t> indices;
        indices.reserve(handles.size());
        for (const auto handle : handles)
        {
            const auto found = m_slotByProxyId.find(MakeProxyKey(handle));
            if (found != m_slotByProxyId.end())
                indices.push_back(found->second);
        }
        return indices;
    }

    PersistentObjectDataFrame PersistentObjectDataTable::BuildFrame() const
    {
        PersistentObjectDataFrame frame;
        frame.objectData.resize(m_capacity, Maths::Matrix4::Identity);
        for (uint32_t slotIndex = 0u; slotIndex < m_capacity; ++slotIndex)
        {
            const auto& slot = m_slots[slotIndex];
            if (!slot.occupied)
                continue;
            frame.objectData[slotIndex] = slot.modelMatrix;
            if (!slot.dirty)
                continue;

            if (!frame.dirtyRanges.empty() && frame.dirtyRanges.back().end == slotIndex)
                frame.dirtyRanges.back().end = slotIndex + 1u;
            else
                frame.dirtyRanges.push_back({ slotIndex, slotIndex + 1u });
        }
        return frame;
    }

    void PersistentObjectDataTable::Clear()
    {
        m_slots.clear();
        m_freeSlots.clear();
        m_slotByProxyId.clear();
        m_capacity = 0u;
    }
}
