#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Rendering/Data/FrameInfo.h"

namespace NLS::Tests::LargeScene
{
    struct TimingSample
    {
        uint64_t syncTimeNs = 0u;
        uint64_t serialVisibilityTimeNs = 0u;
        uint64_t parallelVisibilityTimeNs = 0u;
        uint64_t queueFinalizationTimeNs = 0u;
        uint64_t hzbBuildTimeNs = 0u;
        uint64_t streamingCommitTimeNs = 0u;
    };

    struct TouchedCountSample
    {
        uint64_t registeredPrimitiveCount = 0u;
        uint64_t syncTouchedPrimitiveCount = 0u;
        uint64_t syncFullSweepCount = 0u;
        uint64_t boundsDirtyPrimitiveCount = 0u;
        uint64_t primitiveSlotReuseCount = 0u;
        uint64_t spatialCandidateCount = 0u;
        uint64_t fullScanCandidateCount = 0u;
        uint64_t primitiveRecordsTouched = 0u;
        uint64_t allocatedPrimitiveSlotCount = 0u;
        uint64_t tombstonedPrimitiveSlotCount = 0u;
        uint64_t syncSweepTouchedSlotCount = 0u;
        uint64_t visibilityTestedPrimitiveCount = 0u;
        uint64_t finalizationTouchedPrimitiveCount = 0u;
        uint64_t finalizationTouchedCommandCount = 0u;
    };

    struct DrawResidencySample
    {
        uint64_t visiblePrimitiveCount = 0u;
        uint64_t visibleMeshCount = 0u;
        uint64_t rawVisibleDrawCount = 0u;
        uint64_t submittedDrawCount = 0u;
        uint64_t dynamicInstanceGroupCount = 0u;
        uint64_t streamingDependencyCount = 0u;
        uint64_t residencyTicketCount = 0u;
        uint64_t residentCpuBytes = 0u;
        uint64_t residentGpuBytes = 0u;
        uint64_t requestedCpuBytes = 0u;
        uint64_t requestedGpuBytes = 0u;
    };

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

    [[nodiscard]] bool AreTouchedCountsBounded(
        const TouchedCountSample& sample,
        double maxCandidateRatio,
        double maxVisibilityTestedRatio);

    [[nodiscard]] std::string FormatTouchedCountTableRow(
        uint64_t frameIndex,
        const TouchedCountSample& sample);

    [[nodiscard]] std::string FormatTimingTableRow(
        uint64_t frameIndex,
        const TimingSample& sample);

    [[nodiscard]] std::string FormatDrawResidencyTableRow(
        uint64_t frameIndex,
        const DrawResidencySample& sample);

    void ExpectTouchedTelemetry(
        const NLS::Render::Data::LargeSceneTelemetry& telemetry,
        const TouchedCountSample& expected);

    void ExpectDrawResidencyTelemetry(
        const NLS::Render::Data::LargeSceneTelemetry& telemetry,
        const DrawResidencySample& expected);
}
