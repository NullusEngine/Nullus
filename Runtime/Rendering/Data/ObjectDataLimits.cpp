#include "Rendering/Data/ObjectDataLimits.h"

namespace NLS::Render::Data
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    namespace
    {
        uint32_t& ObjectDataCountLimitStorage()
        {
            static uint32_t limit = kMaxObjectDataCount;
            return limit;
        }

        uint32_t& ObjectDataCountPerDrawLimitStorage()
        {
            static uint32_t limit = kMaxObjectDataCount;
            return limit;
        }
    }

    uint32_t GetObjectDataCountLimitForTesting()
    {
        return ObjectDataCountLimitStorage();
    }

    void SetObjectDataCountLimitForTesting(const uint32_t limit)
    {
        ObjectDataCountLimitStorage() = limit == 0u ? 1u : limit;
    }

    void ResetObjectDataCountLimitForTesting()
    {
        ObjectDataCountLimitStorage() = kMaxObjectDataCount;
    }

    uint32_t GetObjectDataCountPerDrawLimitForTesting()
    {
        return ObjectDataCountPerDrawLimitStorage();
    }

    void SetObjectDataCountPerDrawLimitForTesting(const uint32_t limit)
    {
        ObjectDataCountPerDrawLimitStorage() = limit == 0u ? 1u : limit;
    }

    void ResetObjectDataCountPerDrawLimitForTesting()
    {
        ObjectDataCountPerDrawLimitStorage() = kMaxObjectDataCount;
    }
#endif
}
