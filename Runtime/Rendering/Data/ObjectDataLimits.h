#pragma once

#include <cstdint>

namespace NLS::Render::Data
{
    inline constexpr uint32_t kMaxObjectDataCount = 1u << 20u;

    inline bool TryResolveObjectDataRangeEnd(
        const uint32_t objectIndex,
        const uint32_t objectCount,
        uint32_t& outLastObjectIndex)
    {
        if (objectCount == 0u)
            return false;

        const uint64_t lastObjectIndex =
            static_cast<uint64_t>(objectIndex) + static_cast<uint64_t>(objectCount) - 1u;
        if (lastObjectIndex >= kMaxObjectDataCount)
            return false;

        outLastObjectIndex = static_cast<uint32_t>(lastObjectIndex);
        return true;
    }
}
