#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <Rendering/Data/FrameInfo.h>

namespace NLS::Editor::Core
{
    struct EditorCameraPerformanceMetadata
    {
        std::string configuration;
        std::string backend;
        bool vsync = false;
        uint32_t warmupFrameCount = 30u;
        uint32_t requestedFrameCount = 300u;
        uint32_t requestedSettleFrameCount = 0u;
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
        Render::Data::LargeSceneTelemetry largeScene;
    };

    struct EditorCameraPerformanceSummary
    {
        EditorCameraPerformanceMetadata metadata;
        std::vector<double> measuredFrameMs;
        std::vector<double> settleFrameMs;
        double meanFrameMs = 0.0;
        double meanFps = 0.0;
        double p95FrameMs = 0.0;
        double p99FrameMs = 0.0;
        double maxFrameMs = 0.0;
        double settleMeanFrameMs = 0.0;
        double settleMeanFps = 0.0;
        double settleP95FrameMs = 0.0;
        double settleP99FrameMs = 0.0;
        double settleMaxFrameMs = 0.0;
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
        uint64_t publishedCameraStepCount = 0u,
        std::vector<double> settleFrameMs = {});

    bool WriteEditorCameraPerformanceSummaryJson(
        const std::filesystem::path& outputPath,
        const EditorCameraPerformanceSummary& summary,
        std::string* error = nullptr);

    enum class EditorCameraPerformanceStage : uint32_t
    {
        PreUpdate = 0u,
        SceneViewUpdate,
        EditorPanels,
        RenderUi,
        PresentSwapchain,
        SceneRender,
        SceneRenderBeginFrame,
        SceneRenderDrawFrame,
        SceneRenderEndFrame,
        SceneRenderDrain,
        Count
    };

    struct EditorCameraPerformanceStageTotals
    {
        static constexpr size_t kStageCount = static_cast<size_t>(EditorCameraPerformanceStage::Count);
        std::array<uint64_t, kStageCount> totalNs {};
        std::array<uint64_t, kStageCount> maxNs {};
        std::array<uint64_t, kStageCount> sampleCount {};
    };

    const char* ToString(EditorCameraPerformanceStage stage);
    void SetEditorCameraPerformanceStageTimingEnabled(bool enabled);
    bool IsEditorCameraPerformanceStageTimingEnabled();
    void ResetEditorCameraPerformanceStageTotals();
    void AccumulateEditorCameraPerformanceStageNs(EditorCameraPerformanceStage stage, uint64_t durationNs);
    EditorCameraPerformanceStageTotals GetEditorCameraPerformanceStageTotals();

    // Main-thread only; no-ops (single branch) when stage timing is disabled.
    class EditorCameraPerformanceStageScope
    {
    public:
        explicit EditorCameraPerformanceStageScope(const EditorCameraPerformanceStage stage)
            : m_stage(stage)
            , m_enabled(IsEditorCameraPerformanceStageTimingEnabled())
        {
            if (m_enabled)
                m_begin = std::chrono::steady_clock::now();
        }

        ~EditorCameraPerformanceStageScope()
        {
            if (!m_enabled)
                return;

            const auto durationNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - m_begin).count());
            AccumulateEditorCameraPerformanceStageNs(m_stage, durationNs);
        }

        EditorCameraPerformanceStageScope(const EditorCameraPerformanceStageScope&) = delete;
        EditorCameraPerformanceStageScope& operator=(const EditorCameraPerformanceStageScope&) = delete;

    private:
        EditorCameraPerformanceStage m_stage;
        bool m_enabled;
        std::chrono::steady_clock::time_point m_begin;
    };
}
