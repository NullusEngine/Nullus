#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace NLS::Editor::Core
{
    inline constexpr auto kStartupSceneRendererResourceStallTimeout = std::chrono::seconds(45);

    inline std::chrono::seconds ResolveStartupSceneRendererResourceHardTimeout(
        const uint32_t effectiveBackgroundWorkerCount)
    {
        const uint32_t workers = std::max(1u, effectiveBackgroundWorkerCount);
        const uint32_t scaledSeconds = 45u + (480u + workers - 1u) / workers;
        return std::chrono::seconds(std::clamp(scaledSeconds, 75u, 300u));
    }

    struct StartupSceneRendererResourcePendingCounts
    {
        size_t taskCount = 0u;
        size_t textureLoadCount = 0u;
        size_t activeStateCount = 0u;
    };

    inline bool HasPendingStartupSceneRendererResources(
        const StartupSceneRendererResourcePendingCounts& counts)
    {
        return counts.taskCount != 0u ||
            counts.textureLoadCount != 0u ||
            counts.activeStateCount != 0u;
    }

    enum class StartupSceneRendererResourceWaitStatus
    {
        Ready,
        Timeout,
        Cancelled
    };

    enum class StartupSceneRendererResourceTimeoutReason
    {
        None,
        HardLimit,
        Stalled
    };

    struct StartupSceneRendererResourceWaitResult
    {
        StartupSceneRendererResourceWaitStatus status = StartupSceneRendererResourceWaitStatus::Ready;
        StartupSceneRendererResourceTimeoutReason timeoutReason =
            StartupSceneRendererResourceTimeoutReason::None;
        uint32_t frameCount = 0u;
        StartupSceneRendererResourcePendingCounts pendingCounts;
        std::chrono::milliseconds elapsed = std::chrono::milliseconds(0);

        bool IsReady() const
        {
            return status == StartupSceneRendererResourceWaitStatus::Ready;
        }
    };

    std::string FormatStartupSceneRendererResourceDegradedOpenDiagnostic(
        const StartupSceneRendererResourceWaitResult& result);

    enum class StartupSceneFinalizationStatus
    {
        Ready,
        Cancelled,
        SubmissionDrainFailed,
        GpuWaitFailed
    };

    template<typename TWaitInitialReady, typename TRunFinalFrame, typename TDrainSubmission,
        typename TWaitStabilizedReady, typename TWaitGpu>
    StartupSceneFinalizationStatus FinalizeStartupSceneBeforeWindow(
        TWaitInitialReady&& waitInitialReady,
        TRunFinalFrame&& runFinalFrame,
        TDrainSubmission&& drainSubmission,
        TWaitStabilizedReady&& waitStabilizedReady,
        TWaitGpu&& waitGpu)
    {
        if (!waitInitialReady())
            return StartupSceneFinalizationStatus::Cancelled;
        runFinalFrame();
        if (!drainSubmission())
            return StartupSceneFinalizationStatus::SubmissionDrainFailed;
        if (!waitStabilizedReady())
            return StartupSceneFinalizationStatus::Cancelled;
        if (!waitGpu())
            return StartupSceneFinalizationStatus::GpuWaitFailed;
        return StartupSceneFinalizationStatus::Ready;
    }

    template<typename TClockNow, typename TIsRunning, typename TGetPendingCounts, typename TPumpFrame,
        typename TPresentProgress, typename TLogProgress, typename TSleep>
    StartupSceneRendererResourceWaitResult WaitForStartupSceneRendererResourcesUntilReady(
        TClockNow&& clockNow,
        TIsRunning&& isRunning,
        TGetPendingCounts&& getPendingCounts,
        TPumpFrame&& pumpFrame,
        TPresentProgress&& presentProgress,
        TLogProgress&& logProgress,
        TSleep&& sleep,
        const std::chrono::milliseconds hardTimeout,
        const std::chrono::milliseconds stallTimeout,
        const std::chrono::milliseconds progressLogInterval,
        const std::chrono::milliseconds progressDialogInterval)
    {
        StartupSceneRendererResourceWaitResult result;
        result.pendingCounts = getPendingCounts();
        if (!HasPendingStartupSceneRendererResources(result.pendingCounts))
            return result;

        const auto waitBegin = clockNow();
        auto lastProgressAt = waitBegin;
        auto progressFrontier = result.pendingCounts;
        auto lastProgressLog = waitBegin;
        auto lastProgressDialog = waitBegin - progressDialogInterval;

        while (isRunning())
        {
            const auto now = clockNow();
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - waitBegin);
            if (result.elapsed >= hardTimeout)
            {
                result.status = StartupSceneRendererResourceWaitStatus::Timeout;
                result.timeoutReason = StartupSceneRendererResourceTimeoutReason::HardLimit;
                return result;
            }

            if (now - lastProgressDialog >= progressDialogInterval)
            {
                presentProgress();
                lastProgressDialog = now;
            }

            pumpFrame();
            ++result.frameCount;
            sleep();

            result.pendingCounts = getPendingCounts();
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clockNow() - waitBegin);
            if (!isRunning())
            {
                result.status = StartupSceneRendererResourceWaitStatus::Cancelled;
                return result;
            }
            if (!HasPendingStartupSceneRendererResources(result.pendingCounts))
            {
                result.status = StartupSceneRendererResourceWaitStatus::Ready;
                return result;
            }

            const bool madeProgress =
                result.pendingCounts.taskCount < progressFrontier.taskCount ||
                result.pendingCounts.textureLoadCount < progressFrontier.textureLoadCount ||
                result.pendingCounts.activeStateCount < progressFrontier.activeStateCount;
            if (madeProgress)
            {
                progressFrontier = result.pendingCounts;
                lastProgressAt = clockNow();
            }
            else
            {
                if (progressFrontier.taskCount == 0u && result.pendingCounts.taskCount != 0u)
                    progressFrontier.taskCount = result.pendingCounts.taskCount;
                if (progressFrontier.textureLoadCount == 0u && result.pendingCounts.textureLoadCount != 0u)
                    progressFrontier.textureLoadCount = result.pendingCounts.textureLoadCount;
                if (progressFrontier.activeStateCount == 0u && result.pendingCounts.activeStateCount != 0u)
                    progressFrontier.activeStateCount = result.pendingCounts.activeStateCount;
            }

            const auto afterFrame = clockNow();
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(afterFrame - waitBegin);
            if (afterFrame - lastProgressAt >= stallTimeout)
            {
                result.status = StartupSceneRendererResourceWaitStatus::Timeout;
                result.timeoutReason = StartupSceneRendererResourceTimeoutReason::Stalled;
                return result;
            }
            if (afterFrame - lastProgressLog >= progressLogInterval)
            {
                logProgress(result.elapsed, result.frameCount, result.pendingCounts);
                lastProgressLog = afterFrame;
            }
        }

        result.status = StartupSceneRendererResourceWaitStatus::Cancelled;
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clockNow() - waitBegin);
        return result;
    }
}
