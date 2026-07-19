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

inline constexpr size_t kAssetThumbnailRenderWorkKindCount =
    static_cast<size_t>(AssetThumbnailRenderWorkKind::Count);

struct AssetThumbnailRenderSchedulerConfig
{
    uint64_t idleInitialBudgetMicroseconds = 2000u;
    uint64_t idleMinimumBudgetMicroseconds = 750u;
    uint64_t idleMaximumBudgetMicroseconds = 3000u;
    uint64_t interactiveInitialBudgetMicroseconds = 500u;
    uint64_t interactiveMinimumBudgetMicroseconds = 250u;
    uint64_t interactiveMaximumBudgetMicroseconds = 1000u;
    uint64_t budgetRecoveryStepMicroseconds = 125u;
    size_t oversizedWorkRetryFrameCount = 8u;
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
    void BeginFrame(uint64_t frameSerial, bool interactive);

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
    [[nodiscard]] uint64_t GetEstimatedWorkMicroseconds(
        AssetThumbnailRenderWorkKind kind) const;
    [[nodiscard]] double GetNextLightGpuPreviewTime() const;
    [[nodiscard]] double GetNextHeavyGpuPreviewTime() const;

private:
    static size_t WorkKindIndex(AssetThumbnailRenderWorkKind kind);
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
    size_t m_startedWorkCount = 0u;
    bool m_interactive = false;
    bool m_frameInitialized = false;
    double m_nextLightGpuPreviewTime = 0.0;
    double m_nextHeavyGpuPreviewTime = 0.0;
};
}
