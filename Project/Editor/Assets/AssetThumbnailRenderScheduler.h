#pragma once

#include "Assets/AssetBrowserPresentation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace NLS::Editor::Assets
{
enum class AssetThumbnailRenderWorkKind
{
    ConsumeCompleted,
    PreviewWarmup,
    GpuPreviewPoll,
    GpuPreviewContinuation,
    LightGpuPreview,
    HeavyGpuPreview,
    BackgroundGenerationStart,
    Count
};

enum class AssetThumbnailRenderScheduleRejection
{
    None,
    FrameNotInitialized,
    ActiveWork,
    PreviousFrameOverTarget,
    InteractiveSuppressed,
    CameraNavigation,
    SceneLoadRendererResourcesPending,
    WorkNotAllowed,
    NextAllowedTime,
    Budget,
    OversizedRetryPending
};

inline constexpr size_t kAssetThumbnailRenderWorkKindCount =
    static_cast<size_t>(AssetThumbnailRenderWorkKind::Count);

struct AssetThumbnailRenderSchedulerConfig
{
    bool adaptiveBudget = true;
    uint64_t idleInitialBudgetMicroseconds = 2000u;
    uint64_t idleMinimumBudgetMicroseconds = 750u;
    uint64_t idleMaximumBudgetMicroseconds = 4000u;
    uint64_t interactiveInitialBudgetMicroseconds = 1000u;
    uint64_t interactiveMinimumBudgetMicroseconds = 250u;
    uint64_t interactiveMaximumBudgetMicroseconds = 1000u;
    uint64_t budgetRecoveryStepMicroseconds = 125u;
    size_t oversizedWorkRetryFrameCount = 8u;
    size_t overBudgetFramesBeforeDowngrade = 30u;
    size_t underBudgetFramesBeforeUpgrade = 60u;
};

struct AssetThumbnailRenderSchedulerFrameStats
{
    uint64_t frameSerial = 0u;
    uint64_t budgetMicroseconds = 0u;
    uint64_t consumedMicroseconds = 0u;
    size_t startedWorkCount = 0u;
    bool interactive = false;
};

/// Coordinates all UI-thread thumbnail work against an adaptive per-frame budget.
class AssetThumbnailRenderScheduler
{
public:
    explicit AssetThumbnailRenderScheduler(
        AssetThumbnailRenderSchedulerConfig config = {});

    /// Starts a budget window; repeated calls for the same frame preserve consumed work.
    void BeginFrame(
        uint64_t frameSerial,
        bool interactive,
        uint64_t previousFrameHeadroomMicroseconds = 0u,
        bool previousFrameOverTarget = false);
    void SetAdaptiveBudgetEnabled(bool enabled);
    [[nodiscard]] bool IsAdaptiveBudgetEnabled() const;

    bool TryBeginCompletedResult();
    bool TryBeginPreviewWarmup(bool allowed);
    bool TryBeginLightGpuPreview(const AssetBrowserLightGpuThumbnailPumpInput& input);
    bool TryBeginHeavyGpuPreview(const AssetBrowserHeavyGpuThumbnailPumpInput& input);
    bool TryBeginBackgroundGeneration(const AssetBrowserThumbnailPumpInput& input);
    /// Records actual cost. Set madeProgress=false for probes that found no work.
    void FinishWork(
        AssetThumbnailRenderWorkKind kind,
        uint64_t elapsedMicroseconds,
        bool madeProgress = true);
    /// Completes the work category selected by the most recent successful TryBegin call.
    void FinishActiveWork(
        uint64_t elapsedMicroseconds,
        bool madeProgress = true);

    void RecordLightGpuPreviewResult(
        bool producedResult,
        double nowSeconds,
        double defaultDelaySeconds);
    void RecordHeavyGpuPreviewResult(
        bool producedResult,
        bool pending,
        std::string_view diagnostic,
        double nowSeconds,
        double resourcePendingDelaySeconds,
        double defaultDelaySeconds);
    void DeferHeavyGpuPreviewUntil(double notBeforeSeconds);

    [[nodiscard]] AssetThumbnailRenderSchedulerFrameStats GetFrameStats() const;
    [[nodiscard]] uint64_t GetConsumedEwmaMicroseconds() const;
    [[nodiscard]] uint64_t GetEstimatedWorkMicroseconds(
        AssetThumbnailRenderWorkKind kind) const;
    [[nodiscard]] double GetNextLightGpuPreviewTime() const;
    [[nodiscard]] double GetNextHeavyGpuPreviewTime() const;
    [[nodiscard]] double GetNextHeavyGpuPreviewContinuationTime() const;
    [[nodiscard]] AssetThumbnailRenderScheduleRejection GetLastRejection() const;
    [[nodiscard]] std::string_view GetLastRejectionName() const;

private:
    static size_t WorkKindIndex(AssetThumbnailRenderWorkKind kind);
    void SetLastRejection(AssetThumbnailRenderScheduleRejection rejection);
    bool TryBeginWork(AssetThumbnailRenderWorkKind kind);
    void AdaptBudgetForCompletedFrame();

    AssetThumbnailRenderSchedulerConfig m_config;
    std::array<uint64_t, kAssetThumbnailRenderWorkKindCount> m_estimatedWorkMicroseconds {
        75u,
        1500u,
        75u,
        750u,
        750u,
        1500u,
        100u
    };
    std::array<size_t, kAssetThumbnailRenderWorkKindCount> m_oversizedWorkRejectedFrameCounts {};
    std::array<uint64_t, kAssetThumbnailRenderWorkKindCount>
        m_lastOversizedWorkRejectedFrameSerials {};
    std::optional<AssetThumbnailRenderWorkKind> m_activeWork;
    uint64_t m_frameSerial = 0u;
    uint64_t m_frameBudgetMicroseconds = 0u;
    uint64_t m_consumedMicroseconds = 0u;
    uint64_t m_idleAdaptiveBudgetMicroseconds = 0u;
    uint64_t m_interactiveAdaptiveBudgetMicroseconds = 0u;
    uint64_t m_consumedEwmaMicroseconds = 0u;
    size_t m_startedWorkCount = 0u;
    size_t m_overBudgetFrameCount = 0u;
    size_t m_underBudgetFrameCount = 0u;
    uint64_t m_previousFrameHeadroomMicroseconds = 0u;
    bool m_previousFrameOverTarget = false;
    bool m_gpuPreviewContinuationStartedThisFrame = false;
    bool m_backgroundGenerationStartedThisFrame = false;
    bool m_interactive = false;
    bool m_frameInitialized = false;
    bool m_adaptiveBudgetEnabled = true;
    double m_nextLightGpuPreviewTime = 0.0;
    double m_nextHeavyGpuPreviewTime = 0.0;
    double m_nextHeavyGpuPreviewContinuationTime = 0.0;
    AssetThumbnailRenderScheduleRejection m_lastRejection =
        AssetThumbnailRenderScheduleRejection::None;
};
}
