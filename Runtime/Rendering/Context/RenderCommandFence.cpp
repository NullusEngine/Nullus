#include "Rendering/Context/RenderCommandFence.h"
#include <atomic>

#include <thread>
#include <algorithm>

#include "Rendering/Context/RenderingThreadSystem.h"

namespace NLS::Render::Context
{
    namespace
    {
        std::atomic<uint64_t> g_unobservedDepthCompletionCount { 0u };

        bool SequenceReached(const uint64_t observed, const uint64_t target)
        {
            // Sequences are monotonic counters starting at zero, so a captured
            // target of zero is already satisfied.
            return target == 0u || observed >= target;
        }
    }

    void RenderCommandFence::BeginFence(const RenderCommandSyncDepth depth)
    {
        m_depth = depth;
        // A new shared block per fence: a marker from a previous BeginFence must
        // not write into this fence's targets.
        m_targets = std::make_shared<TargetState>();
        m_marker = RenderCommandEvent();

        const bool captureSubmit =
            depth == RenderCommandSyncDepth::Rhi || depth == RenderCommandSyncDepth::Swapchain;
        const bool capturePresent = depth == RenderCommandSyncDepth::Swapchain;

        // Capture the shared target block and the marker, never `this`, so the
        // queued command stays valid even if the fence is destroyed first.
        const std::shared_ptr<TargetState> targets = m_targets;
        EnqueueRenderCommand<struct RenderFenceMarkerCommand>(
            [targets, captureSubmit, capturePresent]
            {
                // Runs on the render thread after every command enqueued before
                // BeginFence. The captured target is the amount of work *handed*
                // to the executor by that point, so the fence waits for exactly
                // the submissions that were requested before the marker.
                auto* source = RenderCommandDispatcher::GetSequenceSource();
                if (source != nullptr)
                {
                    if (captureSubmit)
                        targets->submit.store(
                            source->GetDispatchedSequence(),
                            std::memory_order_release);
                    if (capturePresent)
                        targets->present.store(
                            source->GetPresentRequestSequence(),
                            std::memory_order_release);
                }
            },
            m_marker);
    }

    void RenderCommandFence::Wait() const
    {
        (void)WaitFor(std::chrono::milliseconds::max());
    }

    bool RenderCommandFence::WaitFor(const std::chrono::milliseconds timeout) const
    {
        if (m_targets->outcome.IsComplete())
            return true;
        if (!m_marker.IsValid())
        {
            m_targets->outcome.Signal();
            return true;
        }

        const bool infiniteWait = timeout == std::chrono::milliseconds::max();
        const auto deadline = infiniteWait
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + timeout;

        // Stop/failure must release even a marker whose consumer has exited.
        while (!m_marker.IsComplete())
        {
            if (HasRenderFatalError())
            {
                m_marker.Cancel(GetRenderFatalError());
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!infiniteWait && now >= deadline)
                return false;
            (void)m_marker.WaitFor(std::chrono::milliseconds(1));
        }

        if (!m_marker.Succeeded())
        {
            if (m_marker.GetStatus() == RenderCommandStatus::Cancelled)
                m_targets->outcome.Cancel(m_marker.GetDiagnostic());
            else
                m_targets->outcome.Fail(m_marker.GetDiagnostic());
            return true;
        }
        if (m_depth == RenderCommandSyncDepth::Render)
        {
            m_targets->outcome.Signal();
            return true;
        }

        const uint64_t targetSubmit = m_targets->submit.load(std::memory_order_acquire);
        const uint64_t targetPresent = m_targets->present.load(std::memory_order_acquire);

        // Stage two: the captured submissions and presents must have been consumed
        // by the RHI lane. This never waits for the GPU itself.
        while (true)
        {
            auto* source = RenderCommandDispatcher::GetSequenceSource();
            if (source != nullptr)
            {
                const auto diagnostic = source->GetSubmissionFailure((std::max)(targetSubmit, targetPresent));
                if (!diagnostic.empty())
                {
                    m_targets->outcome.Fail(diagnostic);
                    return true;
                }
                const bool submitReached =
                    SequenceReached(source->GetSubmitSequence(), targetSubmit);
                const bool presentReached =
                    m_depth != RenderCommandSyncDepth::Swapchain ||
                    SequenceReached(source->GetPresentSequence(), targetPresent);
                if (submitReached && presentReached)
                {
                    // Failure releases sequence waiters too. Recheck after the
                    // progress reads so that release cannot look like a submit.
                    const auto finalDiagnostic = source->GetSubmissionFailure(
                        (std::max)(targetSubmit, targetPresent));
                    if (finalDiagnostic.empty())
                        m_targets->outcome.Signal();
                    else
                        m_targets->outcome.Fail(finalDiagnostic);
                    return true;
                }
            }

            else
            {
                if (targetSubmit != 0u || targetPresent != 0u)
                {
                    m_targets->outcome.Cancel("RHI executor detached before the render fence completed");
                    return true;
                }
                // No executor is attached: the Render depth is the deepest
                // observable point, so the fence completes here. Count it: the
                // caller asked for a depth this process cannot report, and that
                // must stay measurable instead of looking like a real wait.
                g_unobservedDepthCompletionCount.fetch_add(1u, std::memory_order_relaxed);
                m_targets->outcome.Signal();
                return true;
            }

            if (HasRenderFatalError())
            {
                m_targets->outcome.Fail(GetRenderFatalError());
                return true;
            }

            if (!infiniteWait && std::chrono::steady_clock::now() >= deadline)
                return false;

            (void)RenderingThreadSystem::PumpGameThreadTasks();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool RenderCommandFence::IsFenced() const
    {
        return WaitFor(std::chrono::milliseconds(0));
    }

    bool RenderCommandFence::Succeeded() const
    {
        return IsFenced() && m_targets->outcome.Succeeded();
    }

    std::string RenderCommandFence::GetDiagnostic() const
    {
        (void)IsFenced();
        return m_targets->outcome.GetDiagnostic();
    }

    uint64_t RenderCommandFence::GetUnobservedDepthCompletionCount()
    {
        return g_unobservedDepthCompletionCount.load(std::memory_order_relaxed);
    }

    void RenderCommandFence::ResetUnobservedDepthCompletionCountForTesting()
    {
        g_unobservedDepthCompletionCount.store(0u, std::memory_order_relaxed);
    }
}
