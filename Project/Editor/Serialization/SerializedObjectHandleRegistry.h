#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace NLS::Editor::Serialization
{
class SerializedObject;
using SerializedObjectHandle = uint64_t;

// Opaque managed-facing handles are slot+generation values. Releasing or
// invalidating a slot makes every stale C# property fail without dereferencing
// freed native memory; a recycled slot cannot accidentally resolve an older
// object.
class SerializedObjectHandleRegistry final
{
public:
    SerializedObjectHandleRegistry() = default;
    ~SerializedObjectHandleRegistry() = default;

    SerializedObjectHandleRegistry(const SerializedObjectHandleRegistry&) = delete;
    SerializedObjectHandleRegistry& operator=(const SerializedObjectHandleRegistry&) = delete;

    [[nodiscard]] SerializedObjectHandle Register(std::shared_ptr<SerializedObject> object);
    [[nodiscard]] std::shared_ptr<SerializedObject> Resolve(SerializedObjectHandle handle) const;
    [[nodiscard]] bool Release(SerializedObjectHandle handle);
    void InvalidateAll();

    [[nodiscard]] static uint32_t Slot(SerializedObjectHandle handle) noexcept
    {
        return static_cast<uint32_t>(handle & 0xffff'ffffULL);
    }

    [[nodiscard]] static uint32_t Generation(SerializedObjectHandle handle) noexcept
    {
        return static_cast<uint32_t>(handle >> 32u);
    }

private:
    struct Entry
    {
        std::shared_ptr<SerializedObject> object;
        uint32_t generation = 1u;
        bool occupied = false;
    };

    [[nodiscard]] static SerializedObjectHandle Encode(uint32_t slot, uint32_t generation) noexcept
    {
        return (static_cast<uint64_t>(generation) << 32u) | slot;
    }

    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
};
} // namespace NLS::Editor::Serialization
