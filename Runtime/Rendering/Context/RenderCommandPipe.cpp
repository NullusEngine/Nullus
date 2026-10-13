#include "Rendering/Context/RenderCommandPipe.h"

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <Debug/Logger.h>

#include "Rendering/Context/RenderCommandFence.h"
#include "Rendering/Context/RenderingThreadSystem.h"
#include "TaskGraph/TaskGraph.h"

namespace NLS::Render::Context
{
    namespace
    {
        struct AsyncCompletion;

        struct DispatcherState final
        {
            RenderCommandPipe mainPipe { "Main Render Pipe" };
            RenderCommandPipe asyncPipe { "Async Render Pipe" };
            std::atomic<IRenderCommandSequenceSource*> sequenceSource { nullptr };
            std::atomic<NLS::Base::TaskGraph::TaskGraph*> taskGraph { nullptr };
            // Async bodies that have not finished yet. FlushToDrain waits on this
            // so a caller cannot observe a drained pipe while pool work is live.
            std::atomic<uint32_t> pendingAsyncCount { 0u };
            std::mutex asyncMutex;
            std::vector<std::weak_ptr<AsyncCompletion>> asyncCompletions;
            // Async dependencies a merge gave up on. Non-zero means some producer
            // never honoured its completion contract.
            std::atomic<uint64_t> abandonedAsyncMergeCount { 0u };
            RenderCommandFlushHook flushHook;
            mutable std::mutex hookMutex;
        };

        DispatcherState& Dispatcher()
        {
            // Intentionally process-lifetime so late render commands cannot touch
            // a destroyed pipe during shutdown.
            static DispatcherState* state = new DispatcherState();
            return *state;
        }

        struct AsyncCompletion final
        {
            RenderCommandEvent event;
            std::atomic_bool finished{false};

            ~AsyncCompletion()
            {
                Finish(RenderCommandStatus::Cancelled, "Async render task was cancelled before execution");
            }

            void Finish(const RenderCommandStatus status, std::string diagnostic = {})
            {
                if (finished.exchange(true, std::memory_order_acq_rel))
                    return;
                Dispatcher().pendingAsyncCount.fetch_sub(1u, std::memory_order_acq_rel);
                if (status == RenderCommandStatus::Succeeded)
                    event.Signal();
                else if (status == RenderCommandStatus::Cancelled)
                    event.Cancel(std::move(diagnostic));
                else
                    event.Fail(std::move(diagnostic));
            }
        };

        RenderCommandFlushHook GetFlushHook()
        {
            DispatcherState& dispatcher = Dispatcher();
            std::lock_guard lock(dispatcher.hookMutex);
            return dispatcher.flushHook;
        }
    }

    struct RenderCommandEvent::State final
    {
        mutable std::mutex mutex;
        std::condition_variable condition;
        RenderCommandStatus status = RenderCommandStatus::Pending;
        std::string diagnostic;
        uint64_t sequence = 0u;
    };

    const char* ToString(const RenderCommandSyncDepth depth)
    {
        switch (depth)
        {
        case RenderCommandSyncDepth::Render: return "Render";
        case RenderCommandSyncDepth::Rhi: return "Rhi";
        case RenderCommandSyncDepth::Swapchain: return "Swapchain";
        default: return "Unknown";
        }
    }

    RenderCommandEvent::RenderCommandEvent()
        : m_state(std::make_shared<State>())
    {
    }

    void RenderCommandEvent::Signal() const
    {
        Complete(RenderCommandStatus::Succeeded, {});
    }

    void RenderCommandEvent::Fail(std::string diagnostic) const
    {
        if (diagnostic.empty())
            diagnostic = "Render command failed";
        Complete(RenderCommandStatus::Failed, std::move(diagnostic));
    }

    void RenderCommandEvent::Cancel(std::string diagnostic) const
    {
        if (diagnostic.empty())
            diagnostic = "Render command was cancelled";
        Complete(RenderCommandStatus::Cancelled, std::move(diagnostic));
    }

    void RenderCommandEvent::Complete(const RenderCommandStatus status, std::string diagnostic) const
    {
        if (m_state == nullptr)
            return;

        {
            std::lock_guard lock(m_state->mutex);
            if (m_state->status != RenderCommandStatus::Pending)
                return;
            m_state->diagnostic = std::move(diagnostic);
            m_state->status = status;
        }
        m_state->condition.notify_all();
    }

    void RenderCommandEvent::Wait() const
    {
        (void)WaitFor(std::chrono::milliseconds::max());
    }

    bool RenderCommandEvent::WaitFor(const std::chrono::milliseconds timeout) const
    {
        if (m_state == nullptr)
            return true;

        const bool infiniteWait = timeout == std::chrono::milliseconds::max();
        const auto deadline = infiniteWait
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() + timeout;
        std::unique_lock lock(m_state->mutex);
        const auto complete = [this] { return m_state->status != RenderCommandStatus::Pending; };
        while (!complete())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            lock.unlock();
            (void)RenderingThreadSystem::PumpGameThreadTasks();
            lock.lock();
            m_state->condition.wait_until(lock,
                (std::min)(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(1)), complete);
        }
        return true;
    }

    bool RenderCommandEvent::IsValid() const noexcept
    {
        return m_state != nullptr;
    }

    bool RenderCommandEvent::IsComplete() const
    {
        if (m_state == nullptr)
            return true;
        std::lock_guard lock(m_state->mutex);
        return m_state->status != RenderCommandStatus::Pending;
    }

    bool RenderCommandEvent::Succeeded() const
    {
        return GetStatus() == RenderCommandStatus::Succeeded;
    }

    RenderCommandStatus RenderCommandEvent::GetStatus() const
    {
        if (m_state == nullptr)
            return RenderCommandStatus::Succeeded;
        std::lock_guard lock(m_state->mutex);
        return m_state->status;
    }

    std::string RenderCommandEvent::GetDiagnostic() const
    {
        if (m_state == nullptr)
            return {};
        std::lock_guard lock(m_state->mutex);
        return m_state->diagnostic;
    }

    uint64_t RenderCommandEvent::GetSequence() const
    {
        if (m_state == nullptr)
            return 0u;
        std::lock_guard lock(m_state->mutex);
        return m_state->sequence;
    }

    void RenderCommandEvent::SetSequence(const uint64_t sequence)
    {
        if (m_state == nullptr)
            return;
        std::lock_guard lock(m_state->mutex);
        m_state->sequence = sequence;
    }

    RenderCommandPipe::RenderCommandPipe(std::string debugName)
        : m_debugName(std::move(debugName))
    {
    }

    void RenderCommandPipe::Enqueue(
        std::function<void()> command,
        RenderCommandEvent completion)
    {
        if (!command)
        {
            completion.Signal();
            return;
        }

        std::unique_lock lock(m_mutex);
        // Check under the queue lock so a fatal-error clear cannot race a new
        // enqueue and leave an unconsumed completion token behind.
        if (HasRenderFatalError())
        {
            lock.unlock();
            completion.Cancel(GetRenderFatalError());
            return;
        }
        m_commands.push_back(Command { std::move(command), std::move(completion) });
    }

    size_t RenderCommandPipe::Drain(const size_t maxCommands)
    {
        // A nested drain must not start the next command before the current one
        // returns, even when both drains run on the render thread.
        {
            std::lock_guard lock(m_drainMutex);
            if (m_draining)
                return 0u;
            m_draining = true;
        }

        struct DrainOwnership
        {
            RenderCommandPipe& pipe;

            ~DrainOwnership()
            {
                std::lock_guard lock(pipe.m_drainMutex);
                pipe.m_draining = false;
            }
        } drainOwnership { *this };

        size_t executedCount = 0u;
        while (maxCommands == 0u || executedCount < maxCommands)
        {
            Command command;
            {
                std::lock_guard lock(m_mutex);
                if (m_commands.empty())
                    break;
                command = std::move(m_commands.front());
                m_commands.pop_front();
            }

            if (HasRenderFatalError())
            {
                command.completion.Cancel(GetRenderFatalError());
                std::lock_guard lock(m_mutex);
                ++m_discardedCount;
                continue;
            }

            // Run outside the pipe lock: a command may enqueue follow-up work.
            try
            {
                command.body();
            }
            catch (const std::exception& error)
            {
                const auto diagnostic = "Render command in " + m_debugName + ": " + error.what();
                command.completion.Fail(diagnostic);
                ReportRenderFatalError(diagnostic);
            }
            catch (...)
            {
                const auto diagnostic = "Render command in " + m_debugName + ": unknown exception";
                command.completion.Fail(diagnostic);
                ReportRenderFatalError(diagnostic);
            }

            if (HasRenderFatalError())
                command.completion.Fail(GetRenderFatalError());
            else
                command.completion.Signal();
            ++executedCount;
            {
                std::lock_guard lock(m_mutex);
                ++m_executedCount;
            }
        }
        return executedCount;
    }

    bool RenderCommandPipe::IsEmpty() const
    {
        std::lock_guard lock(m_mutex);
        return m_commands.empty();
    }

    size_t RenderCommandPipe::GetQueuedCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_commands.size();
    }

    uint64_t RenderCommandPipe::GetExecutedCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_executedCount;
    }

    void RenderCommandPipe::Clear(std::string diagnostic)
    {
        if (diagnostic.empty())
            diagnostic = "Render command pipe was cleared: " + m_debugName;
        std::deque<Command> discarded;
        {
            std::lock_guard lock(m_mutex);
            discarded.swap(m_commands);
            m_discardedCount += discarded.size();
        }

        // Signal outside the lock: a waiter wakes and immediately re-enters the
        // pipe, which would deadlock if the lock were still held here.
        for (auto& command : discarded)
            command.completion.Cancel(diagnostic);
    }

    uint64_t RenderCommandPipe::GetDiscardedCount() const
    {
        std::lock_guard lock(m_mutex);
        return m_discardedCount;
    }

    bool HasFlag(const RhiFlushFlags value, const RhiFlushFlags flag)
    {
        return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0u;
    }

    void RenderCommandDispatcher::SetSequenceSource(IRenderCommandSequenceSource* source)
    {
        Dispatcher().sequenceSource.store(source, std::memory_order_release);
    }

    IRenderCommandSequenceSource* RenderCommandDispatcher::GetSequenceSource()
    {
        return Dispatcher().sequenceSource.load(std::memory_order_acquire);
    }

    void RenderCommandDispatcher::SetTaskGraph(NLS::Base::TaskGraph::TaskGraph* taskGraph)
    {
        Dispatcher().taskGraph.store(taskGraph, std::memory_order_release);
    }

    NLS::Base::TaskGraph::TaskGraph* RenderCommandDispatcher::GetTaskGraph()
    {
        return Dispatcher().taskGraph.load(std::memory_order_acquire);
    }

    void RenderCommandDispatcher::Enqueue(
        std::function<void()> command,
        RenderCommandEvent completion)
    {
        if (HasRenderFatalError())
        {
            completion.Cancel(GetRenderFatalError());
            return;
        }
        Dispatcher().mainPipe.Enqueue(std::move(command), std::move(completion));
    }

    RenderCommandEvent RenderCommandDispatcher::EnqueueAsync(std::function<void()> command)
    {
        RenderCommandEvent completion;
        if (!command)
        {
            completion.Signal();
            return completion;
        }
        if (HasRenderFatalError())
        {
            completion.Cancel(GetRenderFatalError());
            return completion;
        }

        DispatcherState& dispatcher = Dispatcher();
        auto asyncCompletion = std::make_shared<AsyncCompletion>();
        asyncCompletion->event = completion;
        dispatcher.pendingAsyncCount.fetch_add(1u, std::memory_order_acq_rel);
        {
            std::lock_guard lock(dispatcher.asyncMutex);
            std::erase_if(dispatcher.asyncCompletions, [](const auto& entry) { return entry.expired(); });
            dispatcher.asyncCompletions.push_back(asyncCompletion);
            if (HasRenderFatalError())
            {
                completion.Cancel(GetRenderFatalError());
                return completion;
            }
        }

        // The async body always signals the dependency itself, so both the pool
        // path and the inline fallback expose the same completion contract.
        auto body = std::make_shared<std::function<void()>>(
            // `mutable` so the captured completion handle can be signalled; the
            // lambda is only ever invoked once.
            [command = std::move(command), completion, asyncCompletion]() mutable
            {
                if (completion.IsComplete() || HasRenderFatalError())
                {
                    asyncCompletion->Finish(RenderCommandStatus::Cancelled, GetRenderFatalError());
                    return;
                }
                try
                {
                    command();
                }
                catch (const std::exception& error)
                {
                    const auto diagnostic = std::string("Async render command: ") + error.what();
                    completion.Fail(diagnostic);
                    ReportRenderFatalError(diagnostic);
                }
                catch (...)
                {
                    completion.Fail("Async render command: unknown exception");
                    ReportRenderFatalError("Async render command threw");
                }

                asyncCompletion->Finish(HasRenderFatalError()
                    ? RenderCommandStatus::Failed : RenderCommandStatus::Succeeded,
                    GetRenderFatalError());
            });

        NLS::Base::TaskGraph::TaskGraph* const graph =
            dispatcher.taskGraph.load(std::memory_order_acquire);
        if (graph != nullptr && graph->IsStarted())
        {
            const auto handle = graph->Submit(
                [body] { (*body)(); },
                NLS::Base::TaskGraph::NamedThread::Any,
                // Low priority so async preparation never starves frame work.
                NLS::Base::TaskGraph::TaskPriority::Low,
                "RenderCommandDispatcher::Async");
            if (handle.IsValid())
                return completion;

            // Submission rejected: the graph is shutting down, so run inline.
        }

        dispatcher.asyncPipe.Enqueue(
            [body]
            {
                (*body)();
            },
            RenderCommandEvent {});
        return completion;
    }

    size_t RenderCommandDispatcher::GetPendingAsyncCount()
    {
        return Dispatcher().pendingAsyncCount.load(std::memory_order_acquire);
    }

    void RenderCommandDispatcher::MergeAsync(RenderCommandEvent dependency)
    {
        MergeAsync(std::move(dependency), kDefaultAsyncMergeWait);
    }

    void RenderCommandDispatcher::MergeAsync(
        RenderCommandEvent dependency,
        const std::chrono::milliseconds maxWait)
    {
        if (!dependency.IsValid())
            return;

        RenderCommandDispatcher::Enqueue(
            [dependency = std::move(dependency), maxWait]
            {
                // Pump the async pipe while waiting: this pipe model has no second
                // render thread, so a blocking wait would deadlock the merge.
                const auto deadline = std::chrono::steady_clock::now() + maxWait;
                while (!dependency.IsComplete() &&
                       std::chrono::steady_clock::now() < deadline)
                {
                    if (HasRenderFatalError())
                        throw std::runtime_error(GetRenderFatalError());
                    if (Dispatcher().asyncPipe.Drain(0u) == 0u)
                        (void)dependency.WaitFor(std::chrono::milliseconds(1));
                }

                if (dependency.IsComplete())
                {
                    if (!dependency.Succeeded())
                        throw std::runtime_error("Async render dependency failed: " + dependency.GetDiagnostic());
                    return;
                }

                // The producer is gone: its pool was torn down, or the work it was
                // submitted to was cancelled. Waiting longer cannot succeed, and
                // blocking here would wedge every later frame in the serial pipe.
                Dispatcher().abandonedAsyncMergeCount.fetch_add(1u, std::memory_order_relaxed);
                const std::string diagnostic = "Async render dependency timed out";
                dependency.Fail(diagnostic);
                throw std::runtime_error(diagnostic);
            });
    }

    uint64_t RenderCommandDispatcher::GetAbandonedAsyncMergeCount()
    {
        return Dispatcher().abandonedAsyncMergeCount.load(std::memory_order_relaxed);
    }

    size_t RenderCommandDispatcher::Dispatch(const size_t maxCommands)
    {
        if (RenderingThreadSystem::HasActiveRenderThread() && !RenderingThreadSystem::IsInRenderingThread())
            return 0u;
#if defined(NLS_ENABLE_TEST_HOOKS)
        std::unique_ptr<ScopedRenderThreadForTesting> joinedLane;
        if (!RenderingThreadSystem::IsInRenderingThread())
            joinedLane = std::make_unique<ScopedRenderThreadForTesting>();
#endif
        // The main pipe is serial, so it is drained to completion; the async pipe
        // is only pumped when the caller asks for a full drain and no dependency
        // is outstanding. MergeAsync is the supported way to join async work.
        return Dispatcher().mainPipe.Drain(maxCommands);
    }

    size_t RenderCommandDispatcher::FlushToDrain()
    {
        if (RenderingThreadSystem::HasActiveRenderThread() && !RenderingThreadSystem::IsInRenderingThread())
            return 0u;
#if defined(NLS_ENABLE_TEST_HOOKS)
        std::unique_ptr<ScopedRenderThreadForTesting> joinedLane;
        if (!RenderingThreadSystem::IsInRenderingThread())
            joinedLane = std::make_unique<ScopedRenderThreadForTesting>();
#endif
        DispatcherState& dispatcher = Dispatcher();
        size_t total = 0u;
        for (size_t pass = 0u; pass < 64u; ++pass)
        {
            const size_t mainExecuted = dispatcher.mainPipe.Drain(0u);
            const size_t asyncExecuted = dispatcher.asyncPipe.Drain(0u);
            total += mainExecuted + asyncExecuted;

            const bool asyncDrained =
                dispatcher.pendingAsyncCount.load(std::memory_order_acquire) == 0u;
            // "Nothing executed this pass" is not the same as "drained". Another
            // thread may own the main pipe right now and be executing the commands
            // this pass could not claim, so the pipe's own emptiness is the only
            // correct stopping condition. Breaking on the execution count alone
            // returns while commands are still pending.
            const bool mainDrained = dispatcher.mainPipe.IsEmpty();
            if (mainExecuted == 0u && asyncExecuted == 0u && asyncDrained && mainDrained)
                break;

            if (mainExecuted == 0u && asyncExecuted == 0u)
            {
                // Pool work is still running: yield instead of spinning hot.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return total;
    }

    bool RenderCommandDispatcher::IsEmpty()
    {
        return Dispatcher().mainPipe.IsEmpty() && Dispatcher().asyncPipe.IsEmpty();
    }

    size_t RenderCommandDispatcher::GetMainQueueDepth()
    {
        return Dispatcher().mainPipe.GetQueuedCount();
    }

    size_t RenderCommandDispatcher::GetAsyncQueueDepth()
    {
        return Dispatcher().asyncPipe.GetQueuedCount();
    }

    RenderCommandPipe& RenderCommandDispatcher::GetMainPipe()
    {
        return Dispatcher().mainPipe;
    }

    RenderCommandPipe& RenderCommandDispatcher::GetAsyncPipe()
    {
        return Dispatcher().asyncPipe;
    }

    void RenderCommandDispatcher::CancelPendingCommands(const std::string& diagnostic)
    {
        DispatcherState& dispatcher = Dispatcher();
        dispatcher.mainPipe.Clear(diagnostic);
        dispatcher.asyncPipe.Clear(diagnostic);
        std::lock_guard lock(dispatcher.asyncMutex);
        for (const auto& entry : dispatcher.asyncCompletions)
        {
            if (const auto completion = entry.lock())
                completion->event.Cancel(diagnostic);
        }
    }

    void RenderCommandDispatcher::ResetForTesting()
    {
        DispatcherState& dispatcher = Dispatcher();
        CancelPendingCommands("Render command dispatcher was reset");
#if defined(NLS_ENABLE_TEST_HOOKS)
        // The fatal-error sink is process-wide and sticky by design: once a driver
        // reports an unrecoverable rendering failure, every later render command is
        // refused. That is correct for production, but it makes the dispatcher
        // un-testable after any suite that deliberately injects a failure, because
        // Dispatch() then returns 0 for unrelated tests. "Reset for testing" means
        // the process-wide state this dispatcher consults, so clear it here.
        ResetRenderFatalErrorForTesting();
#endif
        dispatcher.sequenceSource.store(nullptr, std::memory_order_release);
        dispatcher.taskGraph.store(nullptr, std::memory_order_release);
        // Running closures still own their count until they finish. Resetting it
        // here would underflow when a pre-reset worker releases its payload.
        dispatcher.abandonedAsyncMergeCount.store(0u, std::memory_order_release);
        std::lock_guard lock(dispatcher.hookMutex);
        dispatcher.flushHook = {};
    }

    void SetRenderCommandFlushHook(RenderCommandFlushHook hook)
    {
        DispatcherState& dispatcher = Dispatcher();
        std::lock_guard lock(dispatcher.hookMutex);
        dispatcher.flushHook = std::move(hook);
    }

    void FlushRenderingCommands(const RhiFlushFlags flags)
    {
        if (!RenderingThreadSystem::IsInGameThread())
            throw std::logic_error("FlushRenderingCommands requires the game thread");

        RenderCommandFence fence;
        const RenderCommandSyncDepth depth =
            HasFlag(flags, RhiFlushFlags::Swapchain)
                ? RenderCommandSyncDepth::Swapchain
                : (HasFlag(flags, RhiFlushFlags::Submit) ||
                           HasFlag(flags, RhiFlushFlags::Translate)
                       ? RenderCommandSyncDepth::Rhi
                       : RenderCommandSyncDepth::Render);
        fence.BeginFence(depth);

        // Inline draining is limited to a stopped/suspended thread system. A slow
        // render thread must retain ownership of scene updates and frame builds.
        while (!fence.WaitFor(std::chrono::milliseconds(1)))
        {
            if (HasRenderFatalError())
                throw std::runtime_error(GetRenderFatalError());
            if (!RenderingThreadSystem::HasActiveRenderThread())
                (void)RenderCommandDispatcher::FlushToDrain();
        }
        if (HasRenderFatalError())
            throw std::runtime_error(GetRenderFatalError());
        if (!fence.Succeeded())
            throw std::runtime_error(fence.GetDiagnostic());

        if (const RenderCommandFlushHook hook = GetFlushHook())
            hook(flags);
    }
}
