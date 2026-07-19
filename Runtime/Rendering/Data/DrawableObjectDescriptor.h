#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include <Math/Matrix4.h>

namespace NLS::Render::Data
{
    inline constexpr uint32_t kDrawableObjectFlagReceiveShadows = 1u << 0u;
    inline constexpr uint32_t kDrawableObjectFlagCastShadows = 1u << 1u;

    struct DrawableObjectDescriptor
    {
        static constexpr uint32_t kInvalidObjectIndex = (std::numeric_limits<uint32_t>::max)();

        Maths::Matrix4 modelMatrix = Maths::Matrix4::Identity;
        Maths::Matrix4 userMatrix = Maths::Matrix4::Identity;
        uint32_t objectIndex = kInvalidObjectIndex;
        uint32_t objectCount = 1u;
        std::vector<Maths::Matrix4> instanceModelMatrices;
        uint32_t objectFlags = kDrawableObjectFlagReceiveShadows |
            kDrawableObjectFlagCastShadows;
        // Stable scene identity used to keep static opaque instance ordering independent of camera distance.
        static constexpr uint64_t kInvalidStableSortKey = (std::numeric_limits<uint64_t>::max)();
        uint64_t stableSortKey = kInvalidStableSortKey;
    };

    struct ObjectDrawConstants
    {
        uint32_t objectIndex = DrawableObjectDescriptor::kInvalidObjectIndex;
        uint32_t objectFlags = kDrawableObjectFlagReceiveShadows |
            kDrawableObjectFlagCastShadows;
        uint32_t padding0 = 0u;
        uint32_t padding1 = 0u;
    };

    static_assert(sizeof(ObjectDrawConstants) == 16u);
}
