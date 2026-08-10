#include "Core/EditorCameraPerformanceBenchmark.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <system_error>
#include <type_traits>

#include <Json/json.hpp>

namespace NLS::Editor::Core
{
namespace
{
    uint64_t SaturatingDelta(const uint64_t before, const uint64_t after)
    {
        return after >= before ? after - before : 0u;
    }

    double NearestRankPercentile(std::vector<double> sortedSamples, const double percentile)
    {
        if (sortedSamples.empty())
            return 0.0;

        std::sort(sortedSamples.begin(), sortedSamples.end());
        const auto oneBasedRank = static_cast<size_t>(
            std::ceil(std::clamp(percentile, 0.0, 1.0) * static_cast<double>(sortedSamples.size())));
        const size_t index = std::clamp<size_t>(oneBasedRank == 0u ? 0u : oneBasedRank - 1u, 0u, sortedSamples.size() - 1u);
        return sortedSamples[index];
    }

    void CalculateFrameMetrics(
        const std::vector<double>& samples,
        double& meanFrameMs,
        double& meanFps,
        double& p95FrameMs,
        double& p99FrameMs,
        double& maxFrameMs)
    {
        if (samples.empty())
            return;

        const double totalMs = std::accumulate(samples.begin(), samples.end(), 0.0);
        meanFrameMs = totalMs / static_cast<double>(samples.size());
        meanFps = meanFrameMs > 0.0 ? 1000.0 / meanFrameMs : 0.0;
        p95FrameMs = NearestRankPercentile(samples, 0.95);
        p99FrameMs = NearestRankPercentile(samples, 0.99);
        maxFrameMs = *std::max_element(samples.begin(), samples.end());
    }

    NLS::Render::Data::LargeSceneTelemetry CalculateLargeSceneTelemetryDelta(
        const NLS::Render::Data::LargeSceneTelemetry& before,
        const NLS::Render::Data::LargeSceneTelemetry& after)
    {
        using LargeSceneTelemetry = NLS::Render::Data::LargeSceneTelemetry;
        constexpr size_t scalarCount =
            NLS::Render::Data::kLargeSceneTelemetryScalarFieldCount +
            NLS::Render::Data::kLargeSceneCullReasonCount +
            NLS::Render::Data::kLargeSceneLodSelectionBucketCount;
        using ScalarFields = std::array<uint64_t, scalarCount>;

        static_assert(std::is_trivially_copyable_v<LargeSceneTelemetry>);
        static_assert(sizeof(ScalarFields) == sizeof(LargeSceneTelemetry));

        ScalarFields beforeFields {};
        ScalarFields afterFields {};
        ScalarFields deltaFields {};
        std::memcpy(beforeFields.data(), &before, sizeof(before));
        std::memcpy(afterFields.data(), &after, sizeof(after));
        for (size_t fieldIndex = 0u; fieldIndex < scalarCount; ++fieldIndex)
            deltaFields[fieldIndex] = SaturatingDelta(beforeFields[fieldIndex], afterFields[fieldIndex]);

        LargeSceneTelemetry delta {};
        std::memcpy(&delta, deltaFields.data(), sizeof(delta));

        // These are gauges rather than cumulative counters in RendererStats.
        delta.residencyTicketCount = after.residencyTicketCount;
        delta.residentCpuBytes = after.residentCpuBytes;
        delta.residentGpuBytes = after.residentGpuBytes;
        delta.requestedCpuBytes = after.requestedCpuBytes;
        delta.requestedGpuBytes = after.requestedGpuBytes;
        return delta;
    }

    nlohmann::json SerializeLargeSceneTelemetry(const NLS::Render::Data::LargeSceneTelemetry& telemetry)
    {
        return {
            { "registeredPrimitiveCount", telemetry.registeredPrimitiveCount },
            { "staticPrimitiveCount", telemetry.staticPrimitiveCount },
            { "dynamicPrimitiveCount", telemetry.dynamicPrimitiveCount },
            { "unclassifiedPrimitiveCount", telemetry.unclassifiedPrimitiveCount },
            { "spatialCandidateCount", telemetry.spatialCandidateCount },
            { "fullScanCandidateCount", telemetry.fullScanCandidateCount },
            { "visiblePrimitiveCount", telemetry.visiblePrimitiveCount },
            { "visibleMeshCount", telemetry.visibleMeshCount },
            { "culledByReason", telemetry.culledByReason },
            { "lodSelectionCount", telemetry.lodSelectionCount },
            { "activeHLODClusterCount", telemetry.activeHLODClusterCount },
            { "occlusionTestCount", telemetry.occlusionTestCount },
            { "occlusionCulledCount", telemetry.occlusionCulledCount },
            { "streamingRequestCount", telemetry.streamingRequestCount },
            { "streamingCommitCount", telemetry.streamingCommitCount },
            { "streamingEvictCount", telemetry.streamingEvictCount },
            { "streamingDependencyCount", telemetry.streamingDependencyCount },
            { "residencyTicketCount", telemetry.residencyTicketCount },
            { "residentCpuBytes", telemetry.residentCpuBytes },
            { "residentGpuBytes", telemetry.residentGpuBytes },
            { "requestedCpuBytes", telemetry.requestedCpuBytes },
            { "requestedGpuBytes", telemetry.requestedGpuBytes },
            { "primitiveRecordsTouched", telemetry.primitiveRecordsTouched },
            { "allocatedPrimitiveSlotCount", telemetry.allocatedPrimitiveSlotCount },
            { "tombstonedPrimitiveSlotCount", telemetry.tombstonedPrimitiveSlotCount },
            { "syncSweepTouchedSlotCount", telemetry.syncSweepTouchedSlotCount },
            { "syncTouchedPrimitiveCount", telemetry.syncTouchedPrimitiveCount },
            { "syncFullSweepCount", telemetry.syncFullSweepCount },
            { "sceneRenderContentRevisionFastPathCount", telemetry.sceneRenderContentRevisionFastPathCount },
            { "boundsDirtyPrimitiveCount", telemetry.boundsDirtyPrimitiveCount },
            { "primitiveSlotReuseCount", telemetry.primitiveSlotReuseCount },
            { "visibilityTestedPrimitiveCount", telemetry.visibilityTestedPrimitiveCount },
            { "visibilityBitsetWordCount", telemetry.visibilityBitsetWordCount },
            { "finalizationTouchedPrimitiveCount", telemetry.finalizationTouchedPrimitiveCount },
            { "finalizationTouchedCommandCount", telemetry.finalizationTouchedCommandCount },
            { "commandOffsetRebuildCount", telemetry.commandOffsetRebuildCount },
            { "rawVisibleDrawCount", telemetry.rawVisibleDrawCount },
            { "submittedDrawCount", telemetry.submittedDrawCount },
            { "dynamicInstanceGroupCount", telemetry.dynamicInstanceGroupCount },
            { "dynamicCandidateCount", telemetry.dynamicCandidateCount },
            { "dynamicRecordsTouched", telemetry.dynamicRecordsTouched },
            { "staticIndexRefitCount", telemetry.staticIndexRefitCount },
            { "staticIndexRebuildCount", telemetry.staticIndexRebuildCount },
            { "staticIndexLastGoodQueryCount", telemetry.staticIndexLastGoodQueryCount },
            { "staticIndexDirtyOverlayCount", telemetry.staticIndexDirtyOverlayCount },
            { "spatialRebuildFallbackCount", telemetry.spatialRebuildFallbackCount },
            { "dynamicIndexUpdateCount", telemetry.dynamicIndexUpdateCount },
            { "syncTimeNs", telemetry.syncTimeNs },
            { "serialVisibilityTimeNs", telemetry.serialVisibilityTimeNs },
            { "parallelVisibilityTimeNs", telemetry.parallelVisibilityTimeNs },
            { "queueFinalizationTimeNs", telemetry.queueFinalizationTimeNs },
            { "visibleDrawableBuildTimeNs", telemetry.visibleDrawableBuildTimeNs },
            { "opaqueQueueFinalizationTimeNs", telemetry.opaqueQueueFinalizationTimeNs },
            { "visibleObjectIndexAssignmentTimeNs", telemetry.visibleObjectIndexAssignmentTimeNs },
            { "hzbBuildTimeNs", telemetry.hzbBuildTimeNs },
            { "hzbHistoryPruneTouchedHandleCount", telemetry.hzbHistoryPruneTouchedHandleCount },
            { "hzbHistoryPruneRemovedHandleCount", telemetry.hzbHistoryPruneRemovedHandleCount },
            { "hzbHistoryPruneRemovedKeyCount", telemetry.hzbHistoryPruneRemovedKeyCount },
            { "hzbHistoryPruneTimeNs", telemetry.hzbHistoryPruneTimeNs },
            { "streamingCommitTimeNs", telemetry.streamingCommitTimeNs }
        };
    }

    nlohmann::json SerializeTelemetry(const EditorCameraPerformanceTelemetry& telemetry)
    {
        return {
            { "blockedPublishCount", telemetry.blockedPublishCount },
            { "publishedFrameCount", telemetry.publishedFrameCount },
            { "reservedSlotWaitCount", telemetry.reservedSlotWaitCount },
            { "reservedSlotWaitTimeoutCount", telemetry.reservedSlotWaitTimeoutCount },
            { "reservedSlotWaitTotalNs", telemetry.reservedSlotWaitTotalNs },
            { "reservedSlotWaitMaxNs", telemetry.reservedSlotWaitMaxNs },
            { "latestPublishedFrameId", telemetry.latestPublishedFrameId },
            { "latestRetiredFrameId", telemetry.latestRetiredFrameId },
            { "preparedStaticBaseCacheHitCount", telemetry.preparedStaticBaseCacheHitCount },
            { "preparedStaticBaseCacheMissCount", telemetry.preparedStaticBaseCacheMissCount },
            { "staticDrawFastPathHitCount", telemetry.staticDrawFastPathHitCount },
            { "staticDrawFastPathMissCount", telemetry.staticDrawFastPathMissCount },
            { "objectDataRevisionHitCount", telemetry.objectDataRevisionHitCount },
            { "objectDataRevisionFallbackCount", telemetry.objectDataRevisionFallbackCount },
            { "objectDataRevisionDescriptorFallbackCount", telemetry.objectDataRevisionDescriptorFallbackCount },
            { "objectDataRevisionMetadataUnavailableCount", telemetry.objectDataRevisionMetadataUnavailableCount },
            { "objectDataRevisionMetadataUninitializedCount", telemetry.objectDataRevisionMetadataUninitializedCount },
            { "objectDataRevisionMetadataMismatchCount", telemetry.objectDataRevisionMetadataMismatchCount },
            { "objectDataRevisionMetadataInvalidCount", telemetry.objectDataRevisionMetadataInvalidCount },
            { "objectDataRevisionStableIdentityMismatchCount", telemetry.objectDataRevisionStableIdentityMismatchCount },
            { "objectDataRevisionTransformMismatchCount", telemetry.objectDataRevisionTransformMismatchCount },
            { "objectDataRevisionObjectIndexMismatchCount", telemetry.objectDataRevisionObjectIndexMismatchCount },
            { "objectDataRevisionObjectCountMismatchCount", telemetry.objectDataRevisionObjectCountMismatchCount },
            { "opaqueSortTokenHitCount", telemetry.opaqueSortTokenHitCount },
            { "opaqueSortTokenRebuildCount", telemetry.opaqueSortTokenRebuildCount },
            { "descriptorAllocationFailureCount", telemetry.descriptorAllocationFailureCount },
            { "objectDataOverflowCount", telemetry.objectDataOverflowCount },
            { "deviceLostCount", telemetry.deviceLostCount },
            { "unsafeGpuQuarantineCount", telemetry.unsafeGpuQuarantineCount },
            { "largeScene", SerializeLargeSceneTelemetry(telemetry.largeScene) }
        };
    }
}

EditorCameraPerformanceTelemetry CalculateEditorCameraPerformanceTelemetryDelta(
    const EditorCameraPerformanceTelemetry& before,
    const EditorCameraPerformanceTelemetry& after)
{
    EditorCameraPerformanceTelemetry delta;
    delta.blockedPublishCount = SaturatingDelta(before.blockedPublishCount, after.blockedPublishCount);
    delta.publishedFrameCount = SaturatingDelta(before.publishedFrameCount, after.publishedFrameCount);
    delta.reservedSlotWaitCount = SaturatingDelta(before.reservedSlotWaitCount, after.reservedSlotWaitCount);
    delta.reservedSlotWaitTimeoutCount = SaturatingDelta(
        before.reservedSlotWaitTimeoutCount,
        after.reservedSlotWaitTimeoutCount);
    delta.reservedSlotWaitTotalNs = SaturatingDelta(before.reservedSlotWaitTotalNs, after.reservedSlotWaitTotalNs);
    delta.reservedSlotWaitMaxNs = after.reservedSlotWaitMaxNs;
    delta.latestPublishedFrameId = after.latestPublishedFrameId;
    delta.latestRetiredFrameId = after.latestRetiredFrameId;
    delta.preparedStaticBaseCacheHitCount = SaturatingDelta(
        before.preparedStaticBaseCacheHitCount,
        after.preparedStaticBaseCacheHitCount);
    delta.preparedStaticBaseCacheMissCount = SaturatingDelta(
        before.preparedStaticBaseCacheMissCount,
        after.preparedStaticBaseCacheMissCount);
    delta.staticDrawFastPathHitCount = SaturatingDelta(
        before.staticDrawFastPathHitCount,
        after.staticDrawFastPathHitCount);
    delta.staticDrawFastPathMissCount = SaturatingDelta(
        before.staticDrawFastPathMissCount,
        after.staticDrawFastPathMissCount);
    delta.objectDataRevisionHitCount = SaturatingDelta(
        before.objectDataRevisionHitCount,
        after.objectDataRevisionHitCount);
    delta.objectDataRevisionFallbackCount = SaturatingDelta(
        before.objectDataRevisionFallbackCount,
        after.objectDataRevisionFallbackCount);
    delta.objectDataRevisionDescriptorFallbackCount = SaturatingDelta(
        before.objectDataRevisionDescriptorFallbackCount,
        after.objectDataRevisionDescriptorFallbackCount);
    delta.objectDataRevisionMetadataUnavailableCount = SaturatingDelta(
        before.objectDataRevisionMetadataUnavailableCount,
        after.objectDataRevisionMetadataUnavailableCount);
    delta.objectDataRevisionMetadataUninitializedCount = SaturatingDelta(
        before.objectDataRevisionMetadataUninitializedCount,
        after.objectDataRevisionMetadataUninitializedCount);
    delta.objectDataRevisionMetadataMismatchCount = SaturatingDelta(
        before.objectDataRevisionMetadataMismatchCount,
        after.objectDataRevisionMetadataMismatchCount);
    delta.objectDataRevisionMetadataInvalidCount = SaturatingDelta(
        before.objectDataRevisionMetadataInvalidCount,
        after.objectDataRevisionMetadataInvalidCount);
    delta.objectDataRevisionStableIdentityMismatchCount = SaturatingDelta(
        before.objectDataRevisionStableIdentityMismatchCount,
        after.objectDataRevisionStableIdentityMismatchCount);
    delta.objectDataRevisionTransformMismatchCount = SaturatingDelta(
        before.objectDataRevisionTransformMismatchCount,
        after.objectDataRevisionTransformMismatchCount);
    delta.objectDataRevisionObjectIndexMismatchCount = SaturatingDelta(
        before.objectDataRevisionObjectIndexMismatchCount,
        after.objectDataRevisionObjectIndexMismatchCount);
    delta.objectDataRevisionObjectCountMismatchCount = SaturatingDelta(
        before.objectDataRevisionObjectCountMismatchCount,
        after.objectDataRevisionObjectCountMismatchCount);
    delta.opaqueSortTokenHitCount = SaturatingDelta(
        before.opaqueSortTokenHitCount,
        after.opaqueSortTokenHitCount);
    delta.opaqueSortTokenRebuildCount = SaturatingDelta(
        before.opaqueSortTokenRebuildCount,
        after.opaqueSortTokenRebuildCount);
    delta.descriptorAllocationFailureCount = SaturatingDelta(
        before.descriptorAllocationFailureCount,
        after.descriptorAllocationFailureCount);
    delta.objectDataOverflowCount = SaturatingDelta(before.objectDataOverflowCount, after.objectDataOverflowCount);
    delta.deviceLostCount = SaturatingDelta(before.deviceLostCount, after.deviceLostCount);
    delta.unsafeGpuQuarantineCount = SaturatingDelta(
        before.unsafeGpuQuarantineCount,
        after.unsafeGpuQuarantineCount);
    delta.largeScene = CalculateLargeSceneTelemetryDelta(before.largeScene, after.largeScene);
    return delta;
}

EditorCameraPerformanceSummary BuildEditorCameraPerformanceSummary(
    EditorCameraPerformanceMetadata metadata,
    std::vector<double> measuredFrameMs,
    const EditorCameraPerformanceTelemetry& telemetryBefore,
    const EditorCameraPerformanceTelemetry& telemetryAfter,
    const uint64_t publishedCameraStepCount,
    std::vector<double> settleFrameMs)
{
    EditorCameraPerformanceSummary summary;
    summary.metadata = std::move(metadata);
    summary.measuredFrameMs = std::move(measuredFrameMs);
    summary.settleFrameMs = std::move(settleFrameMs);
    summary.telemetryDelta = CalculateEditorCameraPerformanceTelemetryDelta(telemetryBefore, telemetryAfter);
    summary.publishedCameraStepCount = publishedCameraStepCount;
    const auto measuredFrameCount = static_cast<uint64_t>(summary.measuredFrameMs.size());
    summary.telemetryDelta.blockedPublishCount = SaturatingDelta(publishedCameraStepCount, measuredFrameCount);

    CalculateFrameMetrics(
        summary.measuredFrameMs,
        summary.meanFrameMs,
        summary.meanFps,
        summary.p95FrameMs,
        summary.p99FrameMs,
        summary.maxFrameMs);
    CalculateFrameMetrics(
        summary.settleFrameMs,
        summary.settleMeanFrameMs,
        summary.settleMeanFps,
        summary.settleP95FrameMs,
        summary.settleP99FrameMs,
        summary.settleMaxFrameMs);
    if (!summary.measuredFrameMs.empty())
    {
        summary.publicationRatio = static_cast<double>(publishedCameraStepCount) /
            static_cast<double>(summary.measuredFrameMs.size());
    }
    return summary;
}

bool WriteEditorCameraPerformanceSummaryJson(
    const std::filesystem::path& outputPath,
    const EditorCameraPerformanceSummary& summary,
    std::string* error)
{
    const auto fail = [error](std::string message)
    {
        if (error != nullptr)
            *error = std::move(message);
        return false;
    };

    if (outputPath.empty())
        return fail("Editor camera performance output path is empty.");

    std::error_code filesystemError;
    if (!outputPath.parent_path().empty())
    {
        std::filesystem::create_directories(outputPath.parent_path(), filesystemError);
        if (filesystemError)
            return fail("Failed to create benchmark output directory: " + filesystemError.message());
    }

    nlohmann::json root {
        { "schemaVersion", 1u },
        { "configuration", summary.metadata.configuration },
        { "backend", summary.metadata.backend },
        { "vsync", summary.metadata.vsync },
        { "warmupFrameCount", summary.metadata.warmupFrameCount },
        { "requestedFrameCount", summary.metadata.requestedFrameCount },
        { "requestedSettleFrameCount", summary.metadata.requestedSettleFrameCount },
        { "projectPath", summary.metadata.projectPath },
        { "scenePath", summary.metadata.scenePath },
        { "viewportWidth", summary.metadata.viewportWidth },
        { "viewportHeight", summary.metadata.viewportHeight },
        { "cameraForwardStep", summary.metadata.cameraForwardStep },
        { "measuredFrameCount", static_cast<uint64_t>(summary.measuredFrameMs.size()) },
        { "measuredFrameMs", summary.measuredFrameMs },
        { "settleFrameCount", static_cast<uint64_t>(summary.settleFrameMs.size()) },
        { "settleFrameMs", summary.settleFrameMs },
        { "meanFrameMs", summary.meanFrameMs },
        { "meanFps", summary.meanFps },
        { "p95FrameMs", summary.p95FrameMs },
        { "p99FrameMs", summary.p99FrameMs },
        { "maxFrameMs", summary.maxFrameMs },
        { "settleMeanFrameMs", summary.settleMeanFrameMs },
        { "settleMeanFps", summary.settleMeanFps },
        { "settleP95FrameMs", summary.settleP95FrameMs },
        { "settleP99FrameMs", summary.settleP99FrameMs },
        { "settleMaxFrameMs", summary.settleMaxFrameMs },
        { "publishedCameraStepCount", summary.publishedCameraStepCount },
        { "publicationRatio", summary.publicationRatio },
        { "telemetryDelta", SerializeTelemetry(summary.telemetryDelta) }
    };

    auto temporaryPath = outputPath;
    temporaryPath += ".tmp";
    {
        std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
            return fail("Failed to open benchmark output: " + temporaryPath.string());
        stream << root.dump(2) << '\n';
        stream.close();
        if (!stream)
            return fail("Failed to write benchmark output: " + temporaryPath.string());
    }

    std::filesystem::remove(outputPath, filesystemError);
    filesystemError.clear();
    std::filesystem::rename(temporaryPath, outputPath, filesystemError);
    if (filesystemError)
    {
        std::filesystem::remove(temporaryPath);
        return fail("Failed to publish benchmark output: " + filesystemError.message());
    }

    if (error != nullptr)
        error->clear();
    return true;
}

namespace
{
    bool g_stageTimingEnabled = false;
    EditorCameraPerformanceStageTotals g_stageTotals;
}

const char* ToString(const EditorCameraPerformanceStage stage)
{
    switch (stage)
    {
    case EditorCameraPerformanceStage::PreUpdate: return "PreUpdate";
    case EditorCameraPerformanceStage::SceneViewUpdate: return "SceneViewUpdate";
    case EditorCameraPerformanceStage::EditorPanels: return "EditorPanels";
    case EditorCameraPerformanceStage::RenderUi: return "RenderUi";
    case EditorCameraPerformanceStage::PresentSwapchain: return "PresentSwapchain";
    case EditorCameraPerformanceStage::SceneRender: return "SceneRender";
    case EditorCameraPerformanceStage::SceneRenderBeginFrame: return "SceneRenderBeginFrame";
    case EditorCameraPerformanceStage::SceneRenderDrawFrame: return "SceneRenderDrawFrame";
    case EditorCameraPerformanceStage::SceneRenderEndFrame: return "SceneRenderEndFrame";
    case EditorCameraPerformanceStage::SceneRenderDrain: return "SceneRenderDrain";
    default: return "Unknown";
    }
}

void SetEditorCameraPerformanceStageTimingEnabled(const bool enabled)
{
    g_stageTimingEnabled = enabled;
}

bool IsEditorCameraPerformanceStageTimingEnabled()
{
    return g_stageTimingEnabled;
}

void ResetEditorCameraPerformanceStageTotals()
{
    g_stageTotals = {};
}

void AccumulateEditorCameraPerformanceStageNs(
    const EditorCameraPerformanceStage stage,
    const uint64_t durationNs)
{
    const auto index = static_cast<size_t>(stage);
    if (index >= EditorCameraPerformanceStageTotals::kStageCount)
        return;

    g_stageTotals.totalNs[index] += durationNs;
    g_stageTotals.maxNs[index] = std::max(g_stageTotals.maxNs[index], durationNs);
    ++g_stageTotals.sampleCount[index];
}

EditorCameraPerformanceStageTotals GetEditorCameraPerformanceStageTotals()
{
    return g_stageTotals;
}
}
