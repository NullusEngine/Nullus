#include "Core/EditorCameraPerformanceBenchmark.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <system_error>

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
            { "unsafeGpuQuarantineCount", telemetry.unsafeGpuQuarantineCount }
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
    return delta;
}

EditorCameraPerformanceSummary BuildEditorCameraPerformanceSummary(
    EditorCameraPerformanceMetadata metadata,
    std::vector<double> measuredFrameMs,
    const EditorCameraPerformanceTelemetry& telemetryBefore,
    const EditorCameraPerformanceTelemetry& telemetryAfter,
    const uint64_t publishedCameraStepCount)
{
    EditorCameraPerformanceSummary summary;
    summary.metadata = std::move(metadata);
    summary.measuredFrameMs = std::move(measuredFrameMs);
    summary.telemetryDelta = CalculateEditorCameraPerformanceTelemetryDelta(telemetryBefore, telemetryAfter);
    summary.publishedCameraStepCount = publishedCameraStepCount;
    const auto measuredFrameCount = static_cast<uint64_t>(summary.measuredFrameMs.size());
    summary.telemetryDelta.blockedPublishCount = SaturatingDelta(publishedCameraStepCount, measuredFrameCount);

    if (summary.measuredFrameMs.empty())
        return summary;

    const double totalMs = std::accumulate(summary.measuredFrameMs.begin(), summary.measuredFrameMs.end(), 0.0);
    summary.meanFrameMs = totalMs / static_cast<double>(summary.measuredFrameMs.size());
    summary.meanFps = summary.meanFrameMs > 0.0 ? 1000.0 / summary.meanFrameMs : 0.0;
    summary.p95FrameMs = NearestRankPercentile(summary.measuredFrameMs, 0.95);
    summary.p99FrameMs = NearestRankPercentile(summary.measuredFrameMs, 0.99);
    summary.maxFrameMs = *std::max_element(summary.measuredFrameMs.begin(), summary.measuredFrameMs.end());
    summary.publicationRatio = static_cast<double>(publishedCameraStepCount) /
        static_cast<double>(summary.measuredFrameMs.size());
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
        { "projectPath", summary.metadata.projectPath },
        { "scenePath", summary.metadata.scenePath },
        { "viewportWidth", summary.metadata.viewportWidth },
        { "viewportHeight", summary.metadata.viewportHeight },
        { "cameraForwardStep", summary.metadata.cameraForwardStep },
        { "measuredFrameCount", static_cast<uint64_t>(summary.measuredFrameMs.size()) },
        { "measuredFrameMs", summary.measuredFrameMs },
        { "meanFrameMs", summary.meanFrameMs },
        { "meanFps", summary.meanFps },
        { "p95FrameMs", summary.p95FrameMs },
        { "p99FrameMs", summary.p99FrameMs },
        { "maxFrameMs", summary.maxFrameMs },
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
}
