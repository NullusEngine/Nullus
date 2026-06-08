#include "LargeSceneOptimizationTestHelpers.h"

#include <algorithm>
#include <sstream>

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

    bool AreTouchedCountsBounded(
        const TouchedCountSample& sample,
        const double maxCandidateRatio,
        const double maxVisibilityTestedRatio)
    {
        return IsCandidateRatioWithinBudget(
                sample.spatialCandidateCount + sample.fullScanCandidateCount,
                sample.registeredPrimitiveCount,
                maxCandidateRatio)
            && IsCandidateRatioWithinBudget(
                sample.visibilityTestedPrimitiveCount,
                sample.registeredPrimitiveCount,
                maxVisibilityTestedRatio);
    }

    std::string FormatTouchedCountTableRow(
        const uint64_t frameIndex,
        const TouchedCountSample& sample)
    {
        std::ostringstream row;
        row << "| " << frameIndex
            << " | " << sample.registeredPrimitiveCount
            << " | " << sample.syncTouchedPrimitiveCount
            << " | " << sample.syncFullSweepCount
            << " | " << sample.boundsDirtyPrimitiveCount
            << " | " << sample.primitiveSlotReuseCount
            << " | " << sample.spatialCandidateCount
            << " | " << sample.fullScanCandidateCount
            << " | " << sample.primitiveRecordsTouched
            << " | " << sample.allocatedPrimitiveSlotCount
            << " | " << sample.tombstonedPrimitiveSlotCount
            << " | " << sample.syncSweepTouchedSlotCount
            << " | " << sample.visibilityTestedPrimitiveCount
            << " | " << sample.finalizationTouchedPrimitiveCount
            << " | " << sample.finalizationTouchedCommandCount
            << " |";
        return row.str();
    }

    std::string FormatTimingTableRow(
        const uint64_t frameIndex,
        const TimingSample& sample)
    {
        std::ostringstream row;
        row << "| " << frameIndex
            << " | " << sample.syncTimeNs
            << " | " << sample.serialVisibilityTimeNs
            << " | " << sample.parallelVisibilityTimeNs
            << " | " << sample.queueFinalizationTimeNs
            << " | " << sample.hzbBuildTimeNs
            << " | " << sample.streamingCommitTimeNs
            << " |";
        return row.str();
    }

    std::string FormatDrawResidencyTableRow(
        const uint64_t frameIndex,
        const DrawResidencySample& sample)
    {
        std::ostringstream row;
        row << "| " << frameIndex
            << " | " << sample.visiblePrimitiveCount
            << " | " << sample.visibleMeshCount
            << " | " << sample.rawVisibleDrawCount
            << " | " << sample.submittedDrawCount
            << " | " << sample.dynamicInstanceGroupCount
            << " | " << sample.streamingDependencyCount
            << " | " << sample.residencyTicketCount
            << " | " << sample.residentCpuBytes
            << " | " << sample.residentGpuBytes
            << " | " << sample.requestedCpuBytes
            << " | " << sample.requestedGpuBytes
            << " |";
        return row.str();
    }

    void ExpectTouchedTelemetry(
        const NLS::Render::Data::LargeSceneTelemetry& telemetry,
        const TouchedCountSample& expected)
    {
        EXPECT_EQ(telemetry.registeredPrimitiveCount, expected.registeredPrimitiveCount);
        EXPECT_EQ(telemetry.syncTouchedPrimitiveCount, expected.syncTouchedPrimitiveCount);
        EXPECT_EQ(telemetry.syncFullSweepCount, expected.syncFullSweepCount);
        EXPECT_EQ(telemetry.boundsDirtyPrimitiveCount, expected.boundsDirtyPrimitiveCount);
        EXPECT_EQ(telemetry.primitiveSlotReuseCount, expected.primitiveSlotReuseCount);
        EXPECT_EQ(telemetry.spatialCandidateCount, expected.spatialCandidateCount);
        EXPECT_EQ(telemetry.fullScanCandidateCount, expected.fullScanCandidateCount);
        EXPECT_EQ(telemetry.primitiveRecordsTouched, expected.primitiveRecordsTouched);
        EXPECT_EQ(telemetry.allocatedPrimitiveSlotCount, expected.allocatedPrimitiveSlotCount);
        EXPECT_EQ(telemetry.tombstonedPrimitiveSlotCount, expected.tombstonedPrimitiveSlotCount);
        EXPECT_EQ(telemetry.syncSweepTouchedSlotCount, expected.syncSweepTouchedSlotCount);
        EXPECT_EQ(telemetry.visibilityTestedPrimitiveCount, expected.visibilityTestedPrimitiveCount);
        EXPECT_EQ(telemetry.finalizationTouchedPrimitiveCount, expected.finalizationTouchedPrimitiveCount);
        EXPECT_EQ(telemetry.finalizationTouchedCommandCount, expected.finalizationTouchedCommandCount);
    }

    void ExpectDrawResidencyTelemetry(
        const NLS::Render::Data::LargeSceneTelemetry& telemetry,
        const DrawResidencySample& expected)
    {
        EXPECT_EQ(telemetry.visiblePrimitiveCount, expected.visiblePrimitiveCount);
        EXPECT_EQ(telemetry.visibleMeshCount, expected.visibleMeshCount);
        EXPECT_EQ(telemetry.rawVisibleDrawCount, expected.rawVisibleDrawCount);
        EXPECT_EQ(telemetry.submittedDrawCount, expected.submittedDrawCount);
        EXPECT_EQ(telemetry.dynamicInstanceGroupCount, expected.dynamicInstanceGroupCount);
        EXPECT_EQ(telemetry.streamingDependencyCount, expected.streamingDependencyCount);
        EXPECT_EQ(telemetry.residencyTicketCount, expected.residencyTicketCount);
        EXPECT_EQ(telemetry.residentCpuBytes, expected.residentCpuBytes);
        EXPECT_EQ(telemetry.residentGpuBytes, expected.residentGpuBytes);
        EXPECT_EQ(telemetry.requestedCpuBytes, expected.requestedCpuBytes);
        EXPECT_EQ(telemetry.requestedGpuBytes, expected.requestedGpuBytes);
    }
}
