#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <Math/Matrix4.h>

#include "Rendering/SceneProxy.h"
#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
    struct ObjectDataDirtyRange
    {
        uint32_t begin = 0u;
        uint32_t end = 0u;
    };

    struct PersistentObjectDataFrame
    {
        std::vector<Maths::Matrix4> objectData;
        std::vector<ObjectDataDirtyRange> dirtyRanges;
    };

    /**
     * CPU shadow for the render-side persistent ObjectData table. Proxy slots
     * remain stable across frames; only changed ranges are reported for upload.
     */
    class NLS_ENGINE_API PersistentObjectDataTable final
    {
    public:
        uint32_t Acquire(SceneProxyHandle handle, const Maths::Matrix4& modelMatrix, uint64_t revision);
        void Release(SceneProxyHandle handle);
        bool Update(SceneProxyHandle handle, const Maths::Matrix4& modelMatrix, uint64_t revision);

        [[nodiscard]] bool TryGetSlot(SceneProxyHandle handle, uint32_t& outSlot) const;
        [[nodiscard]] uint32_t GetCapacity() const noexcept { return m_capacity; }
        [[nodiscard]] std::vector<uint32_t> BuildInstanceIndirection(
            const std::vector<SceneProxyHandle>& handles) const;
        [[nodiscard]] PersistentObjectDataFrame BuildFrame() const;

        void Clear();

    private:
        struct Slot final
        {
            SceneProxyHandle handle;
            Maths::Matrix4 modelMatrix = Maths::Matrix4::Identity;
            uint64_t revision = 0u;
            bool occupied = false;
            bool dirty = false;
        };

        std::vector<Slot> m_slots;
        std::vector<uint32_t> m_freeSlots;
        std::unordered_map<uint64_t, uint32_t> m_slotByProxyId;
        uint32_t m_capacity = 0u;
    };
}
