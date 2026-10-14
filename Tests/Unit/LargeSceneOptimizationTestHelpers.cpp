#include "LargeSceneOptimizationTestHelpers.h"

#include <algorithm>

namespace NLS::Tests::LargeScene
{
    SyntheticPrimitiveScene BuildPartitionedPrimitiveScene(
        const uint32_t gridWidth,
        const uint32_t gridDepth,
        const float spacing,
        const uint32_t dynamicEveryN)
    {
        SyntheticPrimitiveScene scene;
        scene.primitives.reserve(static_cast<size_t>(gridWidth) * static_cast<size_t>(gridDepth));

        uint64_t id = 0u;
        for (uint32_t z = 0u; z < gridDepth; ++z)
        {
            for (uint32_t x = 0u; x < gridWidth; ++x)
            {
                SyntheticPrimitive primitive;
                primitive.id = id;
                primitive.centerX = static_cast<float>(x) * spacing;
                primitive.centerY = 0.0f;
                primitive.centerZ = static_cast<float>(z) * spacing;
                primitive.radius = std::max(0.5f, spacing * 0.25f);
                primitive.layer = (x + z) % 32u;
                primitive.dynamic = dynamicEveryN != 0u && (id % dynamicEveryN) == 0u;

                if (primitive.dynamic)
                    ++scene.dynamicPrimitiveCount;
                else
                    ++scene.staticPrimitiveCount;

                scene.primitives.push_back(primitive);
                ++id;
            }
        }

        return scene;
    }

    SyntheticPrimitiveScene BuildLinearPrimitiveScene(
        const uint32_t primitiveCount,
        const float spacing,
        const uint32_t dynamicEveryN)
    {
        return BuildPartitionedPrimitiveScene(primitiveCount, 1u, spacing, dynamicEveryN);
    }

    bool IsCandidateRatioWithinBudget(
        const uint64_t candidateCount,
        const uint64_t registeredPrimitiveCount,
        const double maxRatio)
    {
        if (registeredPrimitiveCount == 0u)
            return candidateCount == 0u;

        return static_cast<double>(candidateCount) / static_cast<double>(registeredPrimitiveCount) <= maxRatio;
    }

}
