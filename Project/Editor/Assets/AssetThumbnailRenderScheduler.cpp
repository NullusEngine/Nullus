#include "Assets/AssetThumbnailRenderScheduler.h"

#include <algorithm>

namespace NLS::Editor::Assets
{
namespace
{
constexpr uint64_t kSceneAssemblyBackoffMinimumOverrunMicroseconds = 4000u;
// Keep the cost estimate stable across short editor spikes. Budget admission
// still reacts immediately to an over-target frame, while the learned cost
// follows the 60-frame history required by the thumbnail scheduler contract.
constexpr uint64_t kBudgetEwmaWindowFrames = 60u;

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

std::string_view ScheduleRejectionName(
    const AssetThumbnailRenderScheduleRejection rejection)
{
    switch (rejection)
    {
    case AssetThumbnailRenderScheduleRejection::None: return "none";
    case AssetThumbnailRenderScheduleRejection::FrameNotInitialized: return "frame-not-initialized";
    case AssetThumbnailRenderScheduleRejection::ActiveWork: return "active-work";
    case AssetThumbnailRenderScheduleRejection::PreviousFrameOverTarget: return "previous-frame-over-target";
    case AssetThumbnailRenderScheduleRejection::InteractiveSuppressed: return "interactive-suppressed";
    case AssetThumbnailRenderScheduleRejection::CameraNavigation: return "camera-navigation";
    case AssetThumbnailRenderScheduleRejection::SceneLoadRendererResourcesPending:
        return "scene-load-renderer-resources-pending";
    case AssetThumbnailRenderScheduleRejection::WorkNotAllowed: return "work-not-allowed";
    case AssetThumbnailRenderScheduleRejection::NextAllowedTime: return "next-allowed-time";
    case AssetThumbnailRenderScheduleRejection::Budget: return "budget";
    case AssetThumbnailRenderScheduleRejection::OversizedRetryPending:
        return "oversized-retry-pending";
    }
    return "unknown";
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
    , m_adaptiveBudgetEnabled(m_config.adaptiveBudget)
{
    m_lastOversizedWorkRejectedFrameSerials.fill(
        (std::numeric_limits<uint64_t>::max)());
}

void AssetThumbnailRenderScheduler::BeginFrame(
    const uint64_t frameSerial,
    const bool interactive,
    const uint64_t previousFrameHeadroomMicroseconds,
    const bool previousFrameOverTarget)
{
    if (m_frameInitialized && m_frameSerial == frameSerial)
        return;

    if (m_frameInitialized && m_adaptiveBudgetEnabled)
        AdaptBudgetForCompletedFrame();

    if (m_frameInitialized && m_interactive != interactive)
    {
        // Interactive and idle windows have independent targets. Do not carry
        // pressure from one mode into the other.
        m_overBudgetFrameCount = 0u;
        m_underBudgetFrameCount = 0u;
        m_consumedEwmaMicroseconds = 0u;
    }

    m_frameSerial = frameSerial;
    m_interactive = interactive;
    m_frameBudgetMicroseconds = interactive
        ? m_interactiveAdaptiveBudgetMicroseconds
        : m_idleAdaptiveBudgetMicroseconds;
    m_consumedMicroseconds = 0u;
    m_startedWorkCount = 0u;
    m_previousFrameHeadroomMicroseconds = previousFrameHeadroomMicroseconds;
    m_previousFrameOverTarget = previousFrameOverTarget;
    m_gpuPreviewContinuationStartedThisFrame = false;
    m_backgroundGenerationStartedThisFrame = false;
    m_activeWork.reset();
    m_frameInitialized = true;
}

void AssetThumbnailRenderScheduler::SetAdaptiveBudgetEnabled(const bool enabled)
{
    m_adaptiveBudgetEnabled = enabled;
    if (!enabled)
    {
        m_idleAdaptiveBudgetMicroseconds = ClampBudget(
            m_config.idleInitialBudgetMicroseconds,
            m_config.idleMinimumBudgetMicroseconds,
            m_config.idleMaximumBudgetMicroseconds);
        m_interactiveAdaptiveBudgetMicroseconds = ClampBudget(
            m_config.interactiveInitialBudgetMicroseconds,
            m_config.interactiveMinimumBudgetMicroseconds,
            m_config.interactiveMaximumBudgetMicroseconds);
        m_consumedEwmaMicroseconds = 0u;
        m_overBudgetFrameCount = 0u;
        m_underBudgetFrameCount = 0u;
    }
}

bool AssetThumbnailRenderScheduler::IsAdaptiveBudgetEnabled() const
{
    return m_adaptiveBudgetEnabled;
}

bool AssetThumbnailRenderScheduler::TryBeginCompletedResult()
{
    SetLastRejection(AssetThumbnailRenderScheduleRejection::None);
    return TryBeginWork(AssetThumbnailRenderWorkKind::ConsumeCompleted);
}

bool AssetThumbnailRenderScheduler::TryBeginPreviewWarmup(const bool allowed)
{
    SetLastRejection(AssetThumbnailRenderScheduleRejection::None);
    if (!allowed)
    {
        SetLastRejection(AssetThumbnailRenderScheduleRejection::WorkNotAllowed);
        return false;
    }
    return TryBeginWork(AssetThumbnailRenderWorkKind::PreviewWarmup);
}

bool AssetThumbnailRenderScheduler::TryBeginLightGpuPreview(
    const AssetBrowserLightGpuThumbnailPumpInput& input)
{
    SetLastRejection(AssetThumbnailRenderScheduleRejection::None);
    auto scheduledInput = input;
    scheduledInput.nextAllowedSeconds = m_nextLightGpuPreviewTime;
    if (!PlanAssetBrowserLightGpuThumbnailPump(scheduledInput).shouldPump)
    {
        if (input.interactive)
            SetLastRejection(AssetThumbnailRenderScheduleRejection::InteractiveSuppressed);
        else if (!input.allowGpuPreviewStart || !input.hasQueuedWork ||
            !input.hasPreviewRenderer || input.standardPbrShaderPassPrewarmPending)
            SetLastRejection(AssetThumbnailRenderScheduleRejection::WorkNotAllowed);
        else if (input.nowSeconds < input.deferredUntilSeconds ||
            input.nowSeconds < scheduledInput.nextAllowedSeconds)
            SetLastRejection(AssetThumbnailRenderScheduleRejection::NextAllowedTime);
        else
            SetLastRejection(AssetThumbnailRenderScheduleRejection::WorkNotAllowed);
        return false;
    }
    return TryBeginWork(AssetThumbnailRenderWorkKind::LightGpuPreview);
}

bool AssetThumbnailRenderScheduler::TryBeginHeavyGpuPreview(
    const AssetBrowserHeavyGpuThumbnailPumpInput& input)
{
    SetLastRejection(AssetThumbnailRenderScheduleRejection::None);
    auto scheduledInput = input;
    scheduledInput.nextAllowedSeconds = input.hasQueuedReadback
        ? 0.0
        : (input.hasQueuedReadyResidentPreview
            ? 0.0
            : (input.hasQueuedResourceContinuation
                ? m_nextHeavyGpuPreviewContinuationTime
                : m_nextHeavyGpuPreviewTime));
    const auto workKind = input.hasQueuedReadback
        ? AssetThumbnailRenderWorkKind::GpuPreviewPoll
        : (input.hasQueuedVisibleResidentPreview || input.hasQueuedReadyResidentPreview ||
                input.hasQueuedResourceContinuation
            ? AssetThumbnailRenderWorkKind::GpuPreviewContinuation
            : AssetThumbnailRenderWorkKind::HeavyGpuPreview);
    if (!PlanAssetBrowserHeavyGpuThumbnailPump(scheduledInput).shouldPump)
    {
        if (input.interactive)
            SetLastRejection(AssetThumbnailRenderScheduleRejection::InteractiveSuppressed);
        else if (input.sceneViewCameraNavigationActive)
            SetLastRejection(AssetThumbnailRenderScheduleRejection::CameraNavigation);
        else if (workKind == AssetThumbnailRenderWorkKind::HeavyGpuPreview &&
            input.sceneLoadRendererResourcesPending)
        {
            // Preserve the more immediate cadence reason when scene loading is
            // also active. Once cadence allows a submission, expose the
            // scene-load gate as the actionable rejection.
            if (!input.hasQueuedVisibleResidentPreview &&
                (input.nowSeconds < scheduledInput.nextAllowedSeconds ||
                input.nowSeconds < input.deferredUntilSeconds)
                )
            {
                SetLastRejection(AssetThumbnailRenderScheduleRejection::NextAllowedTime);
            }
            else
            {
                SetLastRejection(
                    AssetThumbnailRenderScheduleRejection::SceneLoadRendererResourcesPending);
            }
        }
        else if (input.hasQueuedResourceContinuation &&
            input.nowSeconds < scheduledInput.nextAllowedSeconds)
        {
            SetLastRejection(AssetThumbnailRenderScheduleRejection::NextAllowedTime);
        }
        else if (!input.allowHeavyGpuPreview || !input.hasQueuedWork ||
            !input.hasPreviewRenderer)
        {
            SetLastRejection(AssetThumbnailRenderScheduleRejection::WorkNotAllowed);
        }
        else
            SetLastRejection(AssetThumbnailRenderScheduleRejection::NextAllowedTime);
        return false;
    }

    // Readback retirement and resource continuations must keep advancing after
    // an over-target frame. Delay only a new heavy submission, and feed those
    // rejections into the existing oversized-work retry counter so a scene
    // that is continuously slower than the target cannot starve queued assets.
    if (m_previousFrameOverTarget &&
        workKind == AssetThumbnailRenderWorkKind::HeavyGpuPreview &&
        !input.hasQueuedVisibleResidentPreview &&
        !input.sceneLoadThumbnailEscapeHatchActive)
    {
        const auto index = WorkKindIndex(workKind);
        if (m_config.oversizedWorkRetryFrameCount == 0u)
        {
            SetLastRejection(AssetThumbnailRenderScheduleRejection::PreviousFrameOverTarget);
            return false;
        }
        if (m_lastOversizedWorkRejectedFrameSerials[index] != m_frameSerial)
        {
            m_lastOversizedWorkRejectedFrameSerials[index] = m_frameSerial;
            ++m_oversizedWorkRejectedFrameCounts[index];
        }
        if (m_oversizedWorkRejectedFrameCounts[index] <
            m_config.oversizedWorkRetryFrameCount)
        {
            SetLastRejection(AssetThumbnailRenderScheduleRejection::PreviousFrameOverTarget);
            return false;
        }
    }

    const bool started = TryBeginWork(workKind);
    if (started && input.hasQueuedNonGpuWork &&
        workKind == AssetThumbnailRenderWorkKind::HeavyGpuPreview)
    {
        // A first heavy submission can create a resource continuation during
        // this call. Reserve one bounded background turn for an already queued
        // CPU thumbnail so that the newly created continuation cannot starve it.
        m_gpuPreviewContinuationStartedThisFrame = true;
    }
    return started;
}

bool AssetThumbnailRenderScheduler::TryBeginBackgroundGeneration(
    const AssetBrowserThumbnailPumpInput& input)
{
    SetLastRejection(AssetThumbnailRenderScheduleRejection::None);
    if (!PlanAssetBrowserThumbnailPump(input).shouldStartBackgroundWork)
    {
        SetLastRejection(AssetThumbnailRenderScheduleRejection::WorkNotAllowed);
        return false;
    }
    return TryBeginWork(AssetThumbnailRenderWorkKind::BackgroundGenerationStart);
}

void AssetThumbnailRenderScheduler::FinishWork(
    const AssetThumbnailRenderWorkKind kind,
    const uint64_t elapsedMicroseconds,
    const bool madeProgress)
{
    if (!m_activeWork.has_value() || *m_activeWork != kind)
        return;

    // Readback polling/retirement is a renderer-lifetime operation. It must
    // continue after a heavy preview overruns the UI budget, and its fence
    // wait/retirement cost must not train or consume the submit budget.
    const bool independentReadbackPoll =
        kind == AssetThumbnailRenderWorkKind::GpuPreviewPoll;
    if (!independentReadbackPoll)
    {
        m_consumedMicroseconds += elapsedMicroseconds;
        if (madeProgress)
            ++m_startedWorkCount;
        const auto index = WorkKindIndex(kind);
        const auto sample = (std::max)(uint64_t {1u}, elapsedMicroseconds);
        m_estimatedWorkMicroseconds[index] =
            (m_estimatedWorkMicroseconds[index] * 3u + sample) / 4u;
    }
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
        m_consumedMicroseconds > m_frameBudgetMicroseconds &&
        m_consumedMicroseconds - m_frameBudgetMicroseconds >
            kSceneAssemblyBackoffMinimumOverrunMicroseconds)
    {
        continuationDelay = resourcePendingDelaySeconds;
    }
    const bool resourceContinuation = pending &&
        (diagnostic.rfind("thumbnail-gpu-preview-readback-pending", 0u) == 0u ||
            diagnostic.rfind("thumbnail-gpu-preview-resident-partial", 0u) == 0u ||
            diagnostic.rfind("thumbnail-gpu-preview-resources-pending", 0u) == 0u);
    if (resourceContinuation)
    {
        m_nextHeavyGpuPreviewContinuationTime = nowSeconds + continuationDelay;
        // Preserve the public heavy-preview cooldown for callers that do not
        // yet distinguish the continuation lane. The actual continuation
        // selector uses its dedicated clock above.
        m_nextHeavyGpuPreviewTime = nowSeconds + continuationDelay;
    }
    else
        m_nextHeavyGpuPreviewTime = nowSeconds + continuationDelay;
}

void AssetThumbnailRenderScheduler::DeferHeavyGpuPreviewUntil(
    const double notBeforeSeconds)
{
    m_nextHeavyGpuPreviewTime = (std::max)(m_nextHeavyGpuPreviewTime, notBeforeSeconds);
    m_nextHeavyGpuPreviewContinuationTime =
        (std::max)(m_nextHeavyGpuPreviewContinuationTime, notBeforeSeconds);
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

uint64_t AssetThumbnailRenderScheduler::GetConsumedEwmaMicroseconds() const
{
    return m_consumedEwmaMicroseconds;
}

double AssetThumbnailRenderScheduler::GetNextHeavyGpuPreviewContinuationTime() const
{
    return m_nextHeavyGpuPreviewContinuationTime;
}

AssetThumbnailRenderScheduleRejection AssetThumbnailRenderScheduler::GetLastRejection() const
{
    return m_lastRejection;
}

std::string_view AssetThumbnailRenderScheduler::GetLastRejectionName() const
{
    return ScheduleRejectionName(m_lastRejection);
}

size_t AssetThumbnailRenderScheduler::WorkKindIndex(
    const AssetThumbnailRenderWorkKind kind)
{
    return static_cast<size_t>(kind);
}

void AssetThumbnailRenderScheduler::SetLastRejection(
    const AssetThumbnailRenderScheduleRejection rejection)
{
    m_lastRejection = rejection;
}

bool AssetThumbnailRenderScheduler::TryBeginWork(
    const AssetThumbnailRenderWorkKind kind)
{
    if (!m_frameInitialized)
    {
        SetLastRejection(AssetThumbnailRenderScheduleRejection::FrameNotInitialized);
        return false;
    }
    if (m_activeWork.has_value())
    {
        SetLastRejection(AssetThumbnailRenderScheduleRejection::ActiveWork);
        return false;
    }

    if (kind == AssetThumbnailRenderWorkKind::GpuPreviewPoll)
    {
        // A submitted GPU preview owns renderer resources until its ticket is
        // retired. Polling that ticket is independent of ordinary thumbnail
        // submit/admission budget and must never be blocked by it.
        m_gpuPreviewContinuationStartedThisFrame = true;
        m_activeWork = kind;
        return true;
    }

    const auto estimate = m_estimatedWorkMicroseconds[WorkKindIndex(kind)];
    const auto remaining = m_consumedMicroseconds < m_frameBudgetMicroseconds
        ? m_frameBudgetMicroseconds - m_consumedMicroseconds
        : 0u;
    const bool canBorrowForProgress =
        m_startedWorkCount == 0u && CanBorrowBudgetForForwardProgress(kind);
    const bool canAdvanceBackgroundLane =
        kind == AssetThumbnailRenderWorkKind::BackgroundGenerationStart &&
        m_gpuPreviewContinuationStartedThisFrame &&
        !m_backgroundGenerationStartedThisFrame;
    const bool canAdvanceQueuedContinuation =
        kind == AssetThumbnailRenderWorkKind::GpuPreviewContinuation &&
        !m_gpuPreviewContinuationStartedThisFrame;
    if (estimate > remaining &&
        !canBorrowForProgress &&
        !canAdvanceBackgroundLane &&
        !canAdvanceQueuedContinuation)
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
        SetLastRejection(
            m_oversizedWorkRejectedFrameCounts[index] >= m_config.oversizedWorkRetryFrameCount
                ? AssetThumbnailRenderScheduleRejection::OversizedRetryPending
                : AssetThumbnailRenderScheduleRejection::Budget);
        return false;
    }

    m_oversizedWorkRejectedFrameCounts[WorkKindIndex(kind)] = 0u;
    if (kind == AssetThumbnailRenderWorkKind::GpuPreviewPoll ||
        kind == AssetThumbnailRenderWorkKind::GpuPreviewContinuation)
        m_gpuPreviewContinuationStartedThisFrame = true;
    if (kind == AssetThumbnailRenderWorkKind::BackgroundGenerationStart)
        m_backgroundGenerationStartedThisFrame = true;
    m_activeWork = kind;
    return true;
}

void AssetThumbnailRenderScheduler::AdaptBudgetForCompletedFrame()
{
    if (m_consumedEwmaMicroseconds == 0u)
    {
        m_consumedEwmaMicroseconds = m_consumedMicroseconds;
    }
    else
    {
        m_consumedEwmaMicroseconds = (
            m_consumedEwmaMicroseconds * (kBudgetEwmaWindowFrames - 1u) +
            m_consumedMicroseconds) / kBudgetEwmaWindowFrames;
    }

    auto& adaptiveBudget = m_interactive
        ? m_interactiveAdaptiveBudgetMicroseconds
        : m_idleAdaptiveBudgetMicroseconds;
    const auto minimumBudget = m_interactive
        ? m_config.interactiveMinimumBudgetMicroseconds
        : m_config.idleMinimumBudgetMicroseconds;
    const auto maximumBudget = m_interactive
        ? m_config.interactiveMaximumBudgetMicroseconds
        : m_config.idleMaximumBudgetMicroseconds;

    const bool frameHeadroomConstrained =
        m_previousFrameOverTarget ||
        m_previousFrameHeadroomMicroseconds > 0u &&
        m_previousFrameHeadroomMicroseconds < m_frameBudgetMicroseconds;
    const bool overBudget =
        m_consumedMicroseconds > m_frameBudgetMicroseconds ||
        (m_consumedMicroseconds > 0u &&
            m_consumedEwmaMicroseconds > m_frameBudgetMicroseconds) ||
        frameHeadroomConstrained;
    const bool underBudget =
        !frameHeadroomConstrained &&
        (m_consumedMicroseconds == 0u ||
            m_consumedEwmaMicroseconds < m_frameBudgetMicroseconds / 2u);

    if (overBudget)
    {
        m_underBudgetFrameCount = 0u;
        if (m_overBudgetFrameCount < (std::numeric_limits<size_t>::max)())
            ++m_overBudgetFrameCount;
        if (m_overBudgetFrameCount >= m_config.overBudgetFramesBeforeDowngrade)
        {
            const auto overrun = m_consumedMicroseconds > m_frameBudgetMicroseconds
                ? m_consumedMicroseconds - m_frameBudgetMicroseconds
                : m_frameBudgetMicroseconds - m_previousFrameHeadroomMicroseconds;
            const auto reduction = (std::max)(
                m_config.budgetRecoveryStepMicroseconds,
                overrun / 2u);
            adaptiveBudget = adaptiveBudget > reduction
                ? adaptiveBudget - reduction
                : minimumBudget;
            m_overBudgetFrameCount = 0u;
        }
    }
    else if (underBudget)
    {
        m_overBudgetFrameCount = 0u;
        if (m_underBudgetFrameCount < (std::numeric_limits<size_t>::max)())
            ++m_underBudgetFrameCount;
        if (m_underBudgetFrameCount >= m_config.underBudgetFramesBeforeUpgrade)
        {
            adaptiveBudget = m_config.budgetRecoveryStepMicroseconds <
                    maximumBudget - adaptiveBudget
                ? adaptiveBudget + m_config.budgetRecoveryStepMicroseconds
                : maximumBudget;
            m_underBudgetFrameCount = 0u;
        }
    }
    else
    {
        m_overBudgetFrameCount = 0u;
        m_underBudgetFrameCount = 0u;
    }
    adaptiveBudget = ClampBudget(adaptiveBudget, minimumBudget, maximumBudget);
}
}
