#include "Rendering/Context/FrameLatencyController.h"

#include <algorithm>
#include <utility>

#include <Debug/Logger.h>

#include "Rendering/Context/RenderingThreadSystem.h"

namespace NLS::Render::Context
{
    FrameLatencyController::FrameLatencyController(const uint32_t maxFramesInFlight)
        : m_maxFramesInFlight((std::max)(1u, maxFramesInFlight))
    {
    }

    bool FrameLatencyController::BeginFrame(const std::chrono::nanoseconds maxWait)
    {
        return ReserveFrame(maxWait).has_value();
    }

    uint32_t FrameLatencyController::RefreshCompletedFramesLocked()
    {
        uint32_t completed = 0u;
        while (!m_openFrames.empty())
        {
            const auto& fence = m_openFrames.front().fence;
            if (!fence.has_value() || !fence->IsFenced())
                break;
            const bool succeeded = fence->Succeeded();
            const auto diagnostic = succeeded ? std::string {} : fence->GetDiagnostic();
            m_openFrames.pop_front();
            ++m_retiredFrameCount;
            ++completed;
            if (!succeeded)
            {
                m_failureDiagnostic = diagnostic.empty() ? "Frame latency fence failed" : diagnostic;
                m_stopped = true;
                m_condition.notify_all();
                break;
            }
        }
        return completed;
    }

    std::optional<uint64_t> FrameLatencyController::ReserveFrame(
        const std::chrono::nanoseconds maxWait, const uint32_t reservedFrameCount)
    {
        std::unique_lock lock(m_mutex);
        (void)RefreshCompletedFramesLocked();
        if (m_stopped)
            return std::nullopt;

        // Consumers using the reserved budget must not reduce producer capacity twice.
        const auto hasCapacity = [this, reservedFrameCount]
        {
            if (m_openFrames.size() >= m_maxFramesInFlight)
                return false;
            if (reservedFrameCount == 0u)
                return true;
            const auto producerCount = std::count_if(m_openFrames.begin(), m_openFrames.end(),
                [](const auto& frame) { return !frame.usesReservedCapacity; });
            return static_cast<uint32_t>(producerCount) <
                m_maxFramesInFlight - (std::min)(reservedFrameCount, m_maxFramesInFlight);
        };
        if (!hasCapacity())
        {
            ++m_stalledFrameCount;
            if (maxWait <= std::chrono::nanoseconds::zero())
            {
                ++m_stallTimeoutCount;
                return std::nullopt;
            }
            const auto stallStart = std::chrono::steady_clock::now();
            const auto deadline = maxWait == std::chrono::nanoseconds::max()
                ? std::chrono::steady_clock::time_point::max() : stallStart + maxWait;
            auto nextReport = stallStart + std::chrono::seconds(2);
            bool timedOut = false;
            while (!m_stopped && !hasCapacity())
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                {
                    timedOut = true;
                    break;
                }
                if (now >= nextReport)
                {
                    nextReport = now + std::chrono::seconds(5);
                    NLS_LOG_WARNING("FrameLatencyController: waiting for command fence; openFrames=" +
                        std::to_string(m_openFrames.size()) + " maxFramesInFlight=" +
                        std::to_string(m_maxFramesInFlight));
                }
                lock.unlock();
                (void)RenderingThreadSystem::PumpGameThreadTasks();
                lock.lock();
                (void)RefreshCompletedFramesLocked();
                if (!m_stopped && !hasCapacity())
                    m_condition.wait_until(lock,
                        (std::min)(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
            }
            m_totalStallTimeNanoseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - stallStart).count());
            if (timedOut)
            {
                ++m_stallTimeoutCount;
                return std::nullopt;
            }
            if (m_stopped)
                return std::nullopt;
        }
        const uint64_t id = m_nextFrameId++;
        m_openFrames.push_back({ id, std::nullopt, reservedFrameCount == 0u });
        ++m_openedFrameCount;
        return id;
    }

    bool FrameLatencyController::ArmFrameFence(const uint64_t frameId, RenderCommandFence fence)
    {
        if (!fence.IsValid())
            return false;
        std::lock_guard lock(m_mutex);
        const auto frame = std::find_if(m_openFrames.begin(), m_openFrames.end(),
            [frameId](const auto& candidate) { return candidate.id == frameId; });
        if (frame == m_openFrames.end() || frame->fence.has_value())
            return false;
        frame->fence = std::move(fence);
        m_condition.notify_all();
        return true;
    }

    bool FrameLatencyController::CancelFrame(const uint64_t frameId)
    {
        {
            std::lock_guard lock(m_mutex);
            const auto frame = std::find_if(m_openFrames.begin(), m_openFrames.end(),
                [frameId](const auto& candidate) { return candidate.id == frameId; });
            if (frame == m_openFrames.end())
                return false;
            m_openFrames.erase(frame);
            ++m_retiredFrameCount;
        }
        m_condition.notify_all();
        return true;
    }

    uint32_t FrameLatencyController::PollCompletedFrames()
    {
        std::lock_guard lock(m_mutex);
        const uint32_t completed = RefreshCompletedFramesLocked();
        if (completed != 0u)
            m_condition.notify_all();
        return completed;
    }

    std::string FrameLatencyController::GetFailureDiagnostic() const
    {
        std::lock_guard lock(m_mutex);
        return m_failureDiagnostic;
    }

#if defined(NLS_ENABLE_TEST_HOOKS)
    bool FrameLatencyController::CompleteOldestFrame()
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_openFrames.empty())
                return false;
            m_openFrames.pop_front();
            ++m_retiredFrameCount;
        }
        m_condition.notify_all();
        return true;
    }

    uint32_t FrameLatencyController::ReconcileInFlight(const uint32_t observedInFlight)
    {
        uint32_t droppedCount = 0u;
        {
            std::lock_guard lock(m_mutex);
            while (m_openFrames.size() > observedInFlight)
            {
                m_openFrames.pop_front();
                ++m_retiredFrameCount;
                ++droppedCount;
            }
        }
        if (droppedCount != 0u)
            m_condition.notify_all();
        return droppedCount;
    }
#endif

    void FrameLatencyController::Stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopped = true;
        }
        m_condition.notify_all();
    }

    void FrameLatencyController::Reset()
    {
        {
            std::lock_guard lock(m_mutex);
            m_retiredFrameCount += m_openFrames.size();
            m_openFrames.clear();
            m_stopped = false;
            m_failureDiagnostic.clear();
        }
        m_condition.notify_all();
    }

    void FrameLatencyController::SetMaxFramesInFlight(const uint32_t maxFramesInFlight)
    {
        {
            std::lock_guard lock(m_mutex);
            m_maxFramesInFlight = (std::max)(1u, maxFramesInFlight);
        }
        m_condition.notify_all();
    }

    uint32_t FrameLatencyController::GetMaxFramesInFlight() const
    {
        std::lock_guard lock(m_mutex);
        return m_maxFramesInFlight;
    }

    uint32_t FrameLatencyController::GetInFlightFrameCount() const
    {
        std::lock_guard lock(m_mutex);
        return static_cast<uint32_t>(m_openFrames.size());
    }

    uint64_t FrameLatencyController::GetOpenedFrameCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_openedFrameCount;
    }

    uint64_t FrameLatencyController::GetRetiredFrameCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_retiredFrameCount;
    }

    uint64_t FrameLatencyController::GetStalledFrameCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_stalledFrameCount;
    }

    uint64_t FrameLatencyController::GetStallTimeoutCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_stallTimeoutCount;
    }

    uint64_t FrameLatencyController::GetTotalStallTimeNanoseconds() const
    {
        std::lock_guard lock(m_mutex);
        return m_totalStallTimeNanoseconds;
    }
}
