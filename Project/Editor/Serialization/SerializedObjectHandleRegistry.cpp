#include "Serialization/SerializedObjectHandleRegistry.h"

#include "Serialization/SerializedObject.h"

namespace NLS::Editor::Serialization
{
SerializedObjectHandle SerializedObjectHandleRegistry::Register(
    std::shared_ptr<SerializedObject> object)
{
    if (!object)
        return 0u;
    std::lock_guard lock(m_mutex);
    for (uint32_t index = 0u; index < m_entries.size(); ++index)
    {
        auto& entry = m_entries[index];
        if (!entry.occupied)
        {
            entry.object = std::move(object);
            entry.occupied = true;
            if (entry.generation == 0u)
                entry.generation = 1u;
            return Encode(index + 1u, entry.generation);
        }
    }
    Entry entry;
    entry.object = std::move(object);
    entry.occupied = true;
    m_entries.push_back(std::move(entry));
    return Encode(static_cast<uint32_t>(m_entries.size()), m_entries.back().generation);
}

std::shared_ptr<SerializedObject> SerializedObjectHandleRegistry::Resolve(
    const SerializedObjectHandle handle) const
{
    if (handle == 0u)
        return {};
    const auto encodedSlot = Slot(handle);
    if (encodedSlot == 0u)
        return {};
    const auto index = static_cast<size_t>(encodedSlot - 1u);
    std::lock_guard lock(m_mutex);
    if (index >= m_entries.size())
        return {};
    const auto& entry = m_entries[index];
    if (!entry.occupied || entry.generation != Generation(handle))
        return {};
    return entry.object;
}

bool SerializedObjectHandleRegistry::Release(const SerializedObjectHandle handle)
{
    if (handle == 0u)
        return false;
    const auto encodedSlot = Slot(handle);
    if (encodedSlot == 0u)
        return false;
    const auto index = static_cast<size_t>(encodedSlot - 1u);
    std::lock_guard lock(m_mutex);
    if (index >= m_entries.size())
        return false;
    auto& entry = m_entries[index];
    if (!entry.occupied || entry.generation != Generation(handle))
        return false;
    entry.object.reset();
    entry.occupied = false;
    ++entry.generation;
    if (entry.generation == 0u)
        entry.generation = 1u;
    return true;
}

void SerializedObjectHandleRegistry::InvalidateAll()
{
    std::lock_guard lock(m_mutex);
    for (auto& entry : m_entries)
    {
        entry.object.reset();
        entry.occupied = false;
        ++entry.generation;
        if (entry.generation == 0u)
            entry.generation = 1u;
    }
}
} // namespace NLS::Editor::Serialization
