#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include <Math/Matrix4.h>

namespace NLS::Render::Data
{
    inline constexpr uint32_t kDrawableObjectFlagReceiveShadows = 1u << 0u;
    inline constexpr uint32_t kDrawableObjectFlagCastShadows = 1u << 1u;

    struct StaticDrawSceneIdentity
    {
        uint64_t sceneId = 0u;
        uint32_t primitiveIndex = (std::numeric_limits<uint32_t>::max)();
        uint32_t primitiveGeneration = 0u;

        [[nodiscard]] bool IsValid() const
        {
            return sceneId != 0u &&
                primitiveIndex != (std::numeric_limits<uint32_t>::max)() &&
                primitiveGeneration != 0u;
        }

        bool operator==(const StaticDrawSceneIdentity& rhs) const = default;
    };

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
        static constexpr uint64_t kInvalidOpaqueSortToken = (std::numeric_limits<uint64_t>::max)();
        uint64_t opaqueSortToken = kInvalidOpaqueSortToken;
        static constexpr uint64_t kInvalidStaticDrawGroupIdentity =
            (std::numeric_limits<uint64_t>::max)();
        bool hasTrustedStaticDrawRevision = false;
        StaticDrawSceneIdentity stableSceneIdentity;
        uint64_t cachedCommandBuildSerial = 0u;
        uint64_t meshInstanceId = 0u;
        uint64_t meshContentRevision = 0u;
        uint64_t materialInstanceId = 0u;
        uint64_t materialParameterRevision = 0u;
        uint64_t materialRenderStateRevision = 0u;
        uint64_t materialBindingRevision = 0u;
        uint64_t shaderInstanceId = 0u;
        uint64_t shaderGeneration = 0u;
        uint64_t transformRevision = 0u;
        uint64_t groupIdentity = kInvalidStaticDrawGroupIdentity;
        bool allowsSingleObjectDataReuse = false;
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
