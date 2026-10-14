#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Rendering/Data/FrameInfo.h"

namespace NLS::Tests::LargeScene
{

    struct SyntheticPrimitive
    {
        uint64_t id = 0u;
        float centerX = 0.0f;
        float centerY = 0.0f;
        float centerZ = 0.0f;
        float radius = 1.0f;
        uint32_t layer = 0u;
        bool dynamic = false;
        bool active = true;
    };

    struct SyntheticPrimitiveScene
    {
        std::vector<SyntheticPrimitive> primitives;
        uint64_t staticPrimitiveCount = 0u;
        uint64_t dynamicPrimitiveCount = 0u;
    };

    [[nodiscard]] SyntheticPrimitiveScene BuildPartitionedPrimitiveScene(
        uint32_t gridWidth,
        uint32_t gridDepth,
        float spacing,
        uint32_t dynamicEveryN = 0u);

    [[nodiscard]] SyntheticPrimitiveScene BuildLinearPrimitiveScene(
        uint32_t primitiveCount,
        float spacing,
        uint32_t dynamicEveryN = 0u);

    [[nodiscard]] bool IsCandidateRatioWithinBudget(
        uint64_t candidateCount,
        uint64_t registeredPrimitiveCount,
        double maxRatio);
}
