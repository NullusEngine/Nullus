#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "Serialization/SerializedObjectHandleRegistry.h"

namespace NLS::Editor::Serialization
{
class SerializedProperty;
using SerializedPropertyHandle = uint64_t;

class SerializedPropertyHandleRegistry final
{
public:
    SerializedPropertyHandleRegistry() = default;
    ~SerializedPropertyHandleRegistry() = default;

    SerializedPropertyHandleRegistry(const SerializedPropertyHandleRegistry&) = delete;
    SerializedPropertyHandleRegistry& operator=(const SerializedPropertyHandleRegistry&) = delete;

    [[nodiscard]] SerializedPropertyHandle Register(
        SerializedObjectHandle object,
        std::string path);
    [[nodiscard]] bool Resolve(
        SerializedPropertyHandle handle,
        SerializedObjectHandle& object,
        std::string& path) const;
    [[nodiscard]] bool Release(SerializedPropertyHandle handle);
    void InvalidateObject(SerializedObjectHandle object);
    void InvalidateAll();

    [[nodiscard]] static uint32_t Slot(SerializedPropertyHandle handle) noexcept
    {
        return static_cast<uint32_t>(handle & 0xffff'ffffULL);
    }

    [[nodiscard]] static uint32_t Generation(SerializedPropertyHandle handle) noexcept
    {
        return static_cast<uint32_t>(handle >> 32u);
    }

private:
    struct Entry
    {
        SerializedObjectHandle object = 0u;
        std::string path;
        uint32_t generation = 1u;
        bool occupied = false;
    };

    [[nodiscard]] static SerializedPropertyHandle Encode(
        uint32_t slot,
        uint32_t generation) noexcept
    {
        return (static_cast<uint64_t>(generation) << 32u) | slot;
    }

    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
};
} // namespace NLS::Editor::Serialization
