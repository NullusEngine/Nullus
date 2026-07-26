#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Editor::Core
{
    struct EditorCameraPerformanceMetadata
    {
        std::string configuration;
        std::string backend;
        bool vsync = false;
        uint32_t warmupFrameCount = 30u;
        uint32_t requestedFrameCount = 300u;
        std::string projectPath;
        std::string scenePath;
        uint32_t viewportWidth = 0u;
        uint32_t viewportHeight = 0u;
        double cameraForwardStep = 0.1;
    };

    struct EditorCameraPerformanceTelemetry
    {
        uint64_t blockedPublishCount = 0u;
        uint64_t publishedFrameCount = 0u;
        uint64_t reservedSlotWaitCount = 0u;
        uint64_t reservedSlotWaitTimeoutCount = 0u;
        uint64_t reservedSlotWaitTotalNs = 0u;
        uint64_t reservedSlotWaitMaxNs = 0u;
        uint64_t latestPublishedFrameId = 0u;
        uint64_t latestRetiredFrameId = 0u;
        uint64_t preparedStaticBaseCacheHitCount = 0u;
        uint64_t preparedStaticBaseCacheMissCount = 0u;
        uint64_t staticDrawFastPathHitCount = 0u;
        uint64_t staticDrawFastPathMissCount = 0u;
        uint64_t objectDataRevisionHitCount = 0u;
        uint64_t objectDataRevisionFallbackCount = 0u;
        uint64_t objectDataRevisionDescriptorFallbackCount = 0u;
        uint64_t objectDataRevisionMetadataUnavailableCount = 0u;
        uint64_t objectDataRevisionMetadataUninitializedCount = 0u;
        uint64_t objectDataRevisionMetadataMismatchCount = 0u;
        uint64_t objectDataRevisionMetadataInvalidCount = 0u;
        uint64_t objectDataRevisionStableIdentityMismatchCount = 0u;
        uint64_t objectDataRevisionTransformMismatchCount = 0u;
        uint64_t objectDataRevisionObjectIndexMismatchCount = 0u;
        uint64_t objectDataRevisionObjectCountMismatchCount = 0u;
        uint64_t opaqueSortTokenHitCount = 0u;
        uint64_t opaqueSortTokenRebuildCount = 0u;
        uint64_t descriptorAllocationFailureCount = 0u;
        uint64_t objectDataOverflowCount = 0u;
        uint64_t deviceLostCount = 0u;
        uint64_t unsafeGpuQuarantineCount = 0u;
    };

    struct EditorCameraPerformanceSummary
    {
        EditorCameraPerformanceMetadata metadata;
        std::vector<double> measuredFrameMs;
        double meanFrameMs = 0.0;
        double meanFps = 0.0;
        double p95FrameMs = 0.0;
        double p99FrameMs = 0.0;
        double maxFrameMs = 0.0;
        EditorCameraPerformanceTelemetry telemetryDelta;
        uint64_t publishedCameraStepCount = 0u;
        double publicationRatio = 0.0;
    };

    EditorCameraPerformanceTelemetry CalculateEditorCameraPerformanceTelemetryDelta(
        const EditorCameraPerformanceTelemetry& before,
        const EditorCameraPerformanceTelemetry& after);

    EditorCameraPerformanceSummary BuildEditorCameraPerformanceSummary(
        EditorCameraPerformanceMetadata metadata,
        std::vector<double> measuredFrameMs,
        const EditorCameraPerformanceTelemetry& telemetryBefore,
        const EditorCameraPerformanceTelemetry& telemetryAfter,
        uint64_t publishedCameraStepCount = 0u);

    bool WriteEditorCameraPerformanceSummaryJson(
        const std::filesystem::path& outputPath,
        const EditorCameraPerformanceSummary& summary,
        std::string* error = nullptr);
}
