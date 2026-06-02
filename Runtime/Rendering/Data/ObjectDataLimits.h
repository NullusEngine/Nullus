#pragma once

#include <cstdint>

#include "RenderDef.h"

namespace NLS::Render::Data
{
    inline constexpr uint32_t kMaxObjectDataCount = 1u << 20u;

#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS_RENDER_API uint32_t GetObjectDataCountLimitForTesting();
    NLS_RENDER_API void SetObjectDataCountLimitForTesting(uint32_t limit);
    NLS_RENDER_API void ResetObjectDataCountLimitForTesting();

    NLS_RENDER_API uint32_t GetObjectDataCountPerDrawLimitForTesting();
    NLS_RENDER_API void SetObjectDataCountPerDrawLimitForTesting(uint32_t limit);
    NLS_RENDER_API void ResetObjectDataCountPerDrawLimitForTesting();
#endif

    inline uint32_t GetMaxObjectDataCount()
    {
#if defined(NLS_ENABLE_TEST_HOOKS)
        return GetObjectDataCountLimitForTesting();
#else
        return kMaxObjectDataCount;
#endif
    }

    inline uint32_t GetMaxObjectDataCountPerDraw()
    {
#if defined(NLS_ENABLE_TEST_HOOKS)
        return GetObjectDataCountPerDrawLimitForTesting();
#else
        return kMaxObjectDataCount;
#endif
    }

    inline bool TryResolveObjectDataRangeEnd(
        const uint32_t objectIndex,
        const uint32_t objectCount,
        uint32_t& outLastObjectIndex)
    {
        if (objectCount == 0u)
            return false;

        const uint64_t lastObjectIndex =
            static_cast<uint64_t>(objectIndex) + static_cast<uint64_t>(objectCount) - 1u;
        if (lastObjectIndex >= GetMaxObjectDataCount())
            return false;

        outLastObjectIndex = static_cast<uint32_t>(lastObjectIndex);
        return true;
    }
}
