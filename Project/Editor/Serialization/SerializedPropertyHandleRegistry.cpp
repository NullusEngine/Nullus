#include "Serialization/SerializedPropertyHandleRegistry.h"

namespace NLS::Editor::Serialization
{
SerializedPropertyHandle SerializedPropertyHandleRegistry::Register(
    const SerializedObjectHandle object,
    std::string path)
{
    // Empty paths are reserved for engine-style end-property cursors. They are
    // intentionally not resolvable as values, but still need a stable opaque
    // handle so EqualContents can compare an iterator against its end marker.
    if (object == 0u)
        return 0u;
    std::lock_guard lock(m_mutex);
    for (uint32_t index = 0u; index < m_entries.size(); ++index)
    {
        auto& entry = m_entries[index];
        if (!entry.occupied)
        {
            entry.object = object;
            entry.path = std::move(path);
            entry.occupied = true;
            if (entry.generation == 0u)
                entry.generation = 1u;
            return Encode(index + 1u, entry.generation);
        }
    }

    Entry entry;
    entry.object = object;
    entry.path = std::move(path);
    entry.occupied = true;
    m_entries.push_back(std::move(entry));
    return Encode(static_cast<uint32_t>(m_entries.size()), m_entries.back().generation);
}

bool SerializedPropertyHandleRegistry::Resolve(
    const SerializedPropertyHandle handle,
    SerializedObjectHandle& object,
    std::string& path) const
{
    if (handle == 0u || Slot(handle) == 0u)
        return false;
    const auto index = static_cast<size_t>(Slot(handle) - 1u);
    std::lock_guard lock(m_mutex);
    if (index >= m_entries.size())
        return false;
    const auto& entry = m_entries[index];
    if (!entry.occupied || entry.generation != Generation(handle))
        return false;
    object = entry.object;
    path = entry.path;
    return true;
}

bool SerializedPropertyHandleRegistry::Release(const SerializedPropertyHandle handle)
{
    if (handle == 0u || Slot(handle) == 0u)
        return false;
    const auto index = static_cast<size_t>(Slot(handle) - 1u);
    std::lock_guard lock(m_mutex);
    if (index >= m_entries.size())
        return false;
    auto& entry = m_entries[index];
    if (!entry.occupied || entry.generation != Generation(handle))
        return false;
    entry.object = 0u;
    entry.path.clear();
    entry.occupied = false;
    ++entry.generation;
    if (entry.generation == 0u)
        entry.generation = 1u;
    return true;
}

void SerializedPropertyHandleRegistry::InvalidateObject(const SerializedObjectHandle object)
{
    if (object == 0u)
        return;
    std::lock_guard lock(m_mutex);
    for (auto& entry : m_entries)
    {
        if (!entry.occupied || entry.object != object)
            continue;
        entry.object = 0u;
        entry.path.clear();
        entry.occupied = false;
        ++entry.generation;
        if (entry.generation == 0u)
            entry.generation = 1u;
    }
}

void SerializedPropertyHandleRegistry::InvalidateAll()
{
    std::lock_guard lock(m_mutex);
    for (auto& entry : m_entries)
    {
        entry.object = 0u;
        entry.path.clear();
        entry.occupied = false;
        ++entry.generation;
        if (entry.generation == 0u)
            entry.generation = 1u;
    }
}
} // namespace NLS::Editor::Serialization
