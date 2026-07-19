#include "Assets/AssetThumbnailRenderScheduler.h"

#include <algorithm>

namespace NLS::Editor::Assets
{
namespace
{
AssetThumbnailRenderSchedulerConfig NormalizeConfig(
    AssetThumbnailRenderSchedulerConfig config)
{
    if (config.idleMinimumBudgetMicroseconds > config.idleMaximumBudgetMicroseconds)
    {
        std::swap(
            config.idleMinimumBudgetMicroseconds,
            config.idleMaximumBudgetMicroseconds);
    }
    if (config.interactiveMinimumBudgetMicroseconds >
        config.interactiveMaximumBudgetMicroseconds)
    {
        std::swap(
            config.interactiveMinimumBudgetMicroseconds,
            config.interactiveMaximumBudgetMicroseconds);
    }
    return config;
}

uint64_t ClampBudget(
    const uint64_t value,
    const uint64_t minimum,
    const uint64_t maximum)
{
    return (std::clamp)(value, minimum, maximum);
}

bool CanBorrowBudgetForForwardProgress(const AssetThumbnailRenderWorkKind kind)
{
    return kind == AssetThumbnailRenderWorkKind::ConsumeCompleted ||
        kind == AssetThumbnailRenderWorkKind::GpuPreviewPoll ||
        kind == AssetThumbnailRenderWorkKind::GpuPreviewContinuation ||
        kind == AssetThumbnailRenderWorkKind::BackgroundGenerationStart;
}

bool CanRetryOversizedWork(const AssetThumbnailRenderWorkKind kind)
{
    return kind == AssetThumbnailRenderWorkKind::PreviewWarmup ||
        kind == AssetThumbnailRenderWorkKind::LightGpuPreview ||
        kind == AssetThumbnailRenderWorkKind::GpuPreviewContinuation ||
        kind == AssetThumbnailRenderWorkKind::HeavyGpuPreview;
}
}

AssetThumbnailRenderScheduler::AssetThumbnailRenderScheduler(
    AssetThumbnailRenderSchedulerConfig config)
    : m_config(NormalizeConfig(config))
    , m_idleAdaptiveBudgetMicroseconds(ClampBudget(
          m_config.idleInitialBudgetMicroseconds,
          m_config.idleMinimumBudgetMicroseconds,
          m_config.idleMaximumBudgetMicroseconds))
    , m_interactiveAdaptiveBudgetMicroseconds(ClampBudget(
          m_config.interactiveInitialBudgetMicroseconds,
          m_config.interactiveMinimumBudgetMicroseconds,
          m_config.interactiveMaximumBudgetMicroseconds))
{
    m_lastOversizedWorkRejectedFrameSerials.fill(
        (std::numeric_limits<uint64_t>::max)());
}

void AssetThumbnailRenderScheduler::BeginFrame(
    const uint64_t frameSerial,
    const bool interactive)
{
    if (m_frameInitialized && m_frameSerial == frameSerial)
        return;

    if (m_frameInitialized)
        AdaptBudgetForCompletedFrame();

    m_frameSerial = frameSerial;
    m_interactive = interactive;
    m_frameBudgetMicroseconds = interactive
        ? m_interactiveAdaptiveBudgetMicroseconds
        : m_idleAdaptiveBudgetMicroseconds;
    m_consumedMicroseconds = 0u;
    m_startedWorkCount = 0u;
    m_activeWork.reset();
    m_frameInitialized = true;
}

bool AssetThumbnailRenderScheduler::TryBeginCompletedResult()
{
    return TryBeginWork(AssetThumbnailRenderWorkKind::ConsumeCompleted);
}

bool AssetThumbnailRenderScheduler::TryBeginPreviewWarmup(const bool allowed)
{
    return allowed && TryBeginWork(AssetThumbnailRenderWorkKind::PreviewWarmup);
}

bool AssetThumbnailRenderScheduler::TryBeginLightGpuPreview(
    const AssetBrowserLightGpuThumbnailPumpInput& input)
{
    auto scheduledInput = input;
    scheduledInput.nextAllowedSeconds = m_nextLightGpuPreviewTime;
    return PlanAssetBrowserLightGpuThumbnailPump(scheduledInput).shouldPump &&
        TryBeginWork(AssetThumbnailRenderWorkKind::LightGpuPreview);
}

bool AssetThumbnailRenderScheduler::TryBeginHeavyGpuPreview(
    const AssetBrowserHeavyGpuThumbnailPumpInput& input)
{
    auto scheduledInput = input;
    scheduledInput.nextAllowedSeconds = m_nextHeavyGpuPreviewTime;
    const auto workKind = input.hasQueuedReadback
        ? AssetThumbnailRenderWorkKind::GpuPreviewPoll
        : (input.hasQueuedResourceContinuation
            ? AssetThumbnailRenderWorkKind::GpuPreviewContinuation
            : AssetThumbnailRenderWorkKind::HeavyGpuPreview);
    return PlanAssetBrowserHeavyGpuThumbnailPump(scheduledInput).shouldPump &&
        TryBeginWork(workKind);
}

bool AssetThumbnailRenderScheduler::TryBeginBackgroundGeneration(
    const AssetBrowserThumbnailPumpInput& input)
{
    return PlanAssetBrowserThumbnailPump(input).shouldStartBackgroundWork &&
        TryBeginWork(AssetThumbnailRenderWorkKind::BackgroundGenerationStart);
}

void AssetThumbnailRenderScheduler::FinishWork(
    const AssetThumbnailRenderWorkKind kind,
    const uint64_t elapsedMicroseconds,
    const bool madeProgress)
{
    if (!m_activeWork.has_value() || *m_activeWork != kind)
        return;

    m_consumedMicroseconds += elapsedMicroseconds;
    if (madeProgress)
        ++m_startedWorkCount;
    const auto index = WorkKindIndex(kind);
    const auto sample = (std::max)(uint64_t {1u}, elapsedMicroseconds);
    m_estimatedWorkMicroseconds[index] =
        (m_estimatedWorkMicroseconds[index] * 3u + sample) / 4u;
    m_activeWork.reset();
}

void AssetThumbnailRenderScheduler::FinishActiveWork(
    const uint64_t elapsedMicroseconds,
    const bool madeProgress)
{
    if (!m_activeWork.has_value())
        return;
    const auto kind = *m_activeWork;
    FinishWork(kind, elapsedMicroseconds, madeProgress);
}

void AssetThumbnailRenderScheduler::RecordLightGpuPreviewResult(
    const bool producedResult,
    const double nowSeconds,
    const double defaultDelaySeconds)
{
    if (producedResult)
        m_nextLightGpuPreviewTime = nowSeconds + defaultDelaySeconds;
}

void AssetThumbnailRenderScheduler::RecordHeavyGpuPreviewResult(
    const bool producedResult,
    const bool pending,
    const std::string_view diagnostic,
    const double nowSeconds,
    const double resourcePendingDelaySeconds,
    const double defaultDelaySeconds)
{
    if (!producedResult)
        return;

    auto continuationDelay = PlanAssetBrowserHeavyGpuThumbnailContinuationDelay(
        pending,
        diagnostic,
        resourcePendingDelaySeconds,
        defaultDelaySeconds);
    const bool assemblyContinuation = diagnostic.rfind(
        "thumbnail-gpu-preview-resources-pending:prefab-scene-assembly=",
        0u) == 0u;
    if (assemblyContinuation &&
        continuationDelay == 0.0 &&
        m_frameInitialized &&
        m_consumedMicroseconds > m_frameBudgetMicroseconds)
    {
        continuationDelay = resourcePendingDelaySeconds;
    }
    m_nextHeavyGpuPreviewTime = nowSeconds + continuationDelay;
}

void AssetThumbnailRenderScheduler::DeferHeavyGpuPreviewUntil(
    const double notBeforeSeconds)
{
    m_nextHeavyGpuPreviewTime = (std::max)(m_nextHeavyGpuPreviewTime, notBeforeSeconds);
}

AssetThumbnailRenderSchedulerFrameStats AssetThumbnailRenderScheduler::GetFrameStats() const
{
    return {
        m_frameSerial,
        m_frameBudgetMicroseconds,
        m_consumedMicroseconds,
        m_startedWorkCount,
        m_interactive
    };
}

uint64_t AssetThumbnailRenderScheduler::GetEstimatedWorkMicroseconds(
    const AssetThumbnailRenderWorkKind kind) const
{
    return m_estimatedWorkMicroseconds[WorkKindIndex(kind)];
}

double AssetThumbnailRenderScheduler::GetNextLightGpuPreviewTime() const
{
    return m_nextLightGpuPreviewTime;
}

double AssetThumbnailRenderScheduler::GetNextHeavyGpuPreviewTime() const
{
    return m_nextHeavyGpuPreviewTime;
}

size_t AssetThumbnailRenderScheduler::WorkKindIndex(
    const AssetThumbnailRenderWorkKind kind)
{
    return static_cast<size_t>(kind);
}

bool AssetThumbnailRenderScheduler::TryBeginWork(
    const AssetThumbnailRenderWorkKind kind)
{
    if (!m_frameInitialized || m_activeWork.has_value())
        return false;

    const auto estimate = m_estimatedWorkMicroseconds[WorkKindIndex(kind)];
    const auto remaining = m_consumedMicroseconds < m_frameBudgetMicroseconds
        ? m_frameBudgetMicroseconds - m_consumedMicroseconds
        : 0u;
    const bool canBorrowForProgress =
        m_startedWorkCount == 0u && CanBorrowBudgetForForwardProgress(kind);
    if (estimate > remaining && !canBorrowForProgress)
    {
        const auto index = WorkKindIndex(kind);
        if (!m_interactive &&
            m_config.oversizedWorkRetryFrameCount > 0u &&
            CanRetryOversizedWork(kind))
        {
            if (m_lastOversizedWorkRejectedFrameSerials[index] != m_frameSerial)
            {
                m_lastOversizedWorkRejectedFrameSerials[index] = m_frameSerial;
                ++m_oversizedWorkRejectedFrameCounts[index];
            }
            if (m_startedWorkCount == 0u &&
                m_oversizedWorkRejectedFrameCounts[index] >=
                    m_config.oversizedWorkRetryFrameCount)
            {
                m_oversizedWorkRejectedFrameCounts[index] = 0u;
                m_activeWork = kind;
                return true;
            }
        }
        return false;
    }

    m_oversizedWorkRejectedFrameCounts[WorkKindIndex(kind)] = 0u;
    m_activeWork = kind;
    return true;
}

void AssetThumbnailRenderScheduler::AdaptBudgetForCompletedFrame()
{
    auto& adaptiveBudget = m_interactive
        ? m_interactiveAdaptiveBudgetMicroseconds
        : m_idleAdaptiveBudgetMicroseconds;
    const auto minimumBudget = m_interactive
        ? m_config.interactiveMinimumBudgetMicroseconds
        : m_config.idleMinimumBudgetMicroseconds;
    const auto maximumBudget = m_interactive
        ? m_config.interactiveMaximumBudgetMicroseconds
        : m_config.idleMaximumBudgetMicroseconds;

    if (m_consumedMicroseconds > m_frameBudgetMicroseconds)
    {
        const auto overrun = m_consumedMicroseconds - m_frameBudgetMicroseconds;
        const auto reduction = (std::max)(
            m_config.budgetRecoveryStepMicroseconds,
            overrun / 2u);
        adaptiveBudget = adaptiveBudget > reduction
            ? adaptiveBudget - reduction
            : minimumBudget;
    }
    else if (m_consumedMicroseconds < m_frameBudgetMicroseconds / 2u)
    {
        adaptiveBudget = m_config.budgetRecoveryStepMicroseconds <
                maximumBudget - adaptiveBudget
            ? adaptiveBudget + m_config.budgetRecoveryStepMicroseconds
            : maximumBudget;
    }
    adaptiveBudget = ClampBudget(adaptiveBudget, minimumBudget, maximumBudget);
}
}
