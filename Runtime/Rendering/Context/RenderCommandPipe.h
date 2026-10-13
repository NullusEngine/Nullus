#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "RenderDef.h"

namespace NLS::Base::TaskGraph
{
    class TaskGraph;
}

namespace NLS::Render::Context
{
    /**
     * Sync depth of a render command fence. Mirrors UE's ESyncDepth.
     *
     * Render    - the render thread executed the marker command.
     * Rhi       - everything submitted before the marker reached the GPU queue.
     * Swapchain - everything including the present before the marker was consumed.
     */
    enum class RenderCommandSyncDepth : uint8_t
    {
        Render = 0u,
        Rhi,
        Swapchain
    };

    NLS_RENDER_API const char* ToString(RenderCommandSyncDepth depth);

    enum class RenderCommandStatus : uint8_t
    {
        Pending,
        Succeeded,
        Failed,
        Cancelled
    };

    /**
     * Copyable one-shot completion signal. A command records its completion event
     * when it is enqueued and signals it when it has run, which is how the main
     * pipe and fences observe asynchronous work.
     */
    class NLS_RENDER_API RenderCommandEvent final
    {
    public:
        RenderCommandEvent();

        /**
         * Const because this is a handle to shared completion state: signalling
         * does not mutate the handle, and a caller holding a const token must
         * still be able to release waiters.
         */
        void Signal() const;
        void Fail(std::string diagnostic) const;
        void Cancel(std::string diagnostic) const;
        void Wait() const;
        [[nodiscard]] bool WaitFor(std::chrono::milliseconds timeout) const;
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool IsComplete() const;
        [[nodiscard]] bool Succeeded() const;
        [[nodiscard]] RenderCommandStatus GetStatus() const;
        [[nodiscard]] std::string GetDiagnostic() const;

        [[nodiscard]] uint64_t GetSequence() const;
        void SetSequence(uint64_t sequence);

    private:
        struct State;
        void Complete(RenderCommandStatus status, std::string diagnostic) const;
        std::shared_ptr<State> m_state;
    };

    /**
     * FIFO render command pipe. Commands execute in enqueue order on whichever
     * thread calls Drain, which is the render thread in production.
     * UE equivalent: FRenderCommandPipe / FRenderThreadCommandPipe.
     */
    class NLS_RENDER_API RenderCommandPipe final
    {
    public:
        explicit RenderCommandPipe(std::string debugName);

        void Enqueue(std::function<void()> command, RenderCommandEvent completion = {});

        /** Executes up to maxCommands (0 = all) pending commands on this thread. */
        size_t Drain(size_t maxCommands = 0u);

        [[nodiscard]] bool IsEmpty() const;
        [[nodiscard]] size_t GetQueuedCount() const;
        [[nodiscard]] uint64_t GetExecutedCount() const;

        /**
         * Drops every queued command.
         *
         * Dropped commands never run, so their completion tokens are cancelled
         * here: a fence waiting on a discarded marker would otherwise block
         * forever.
         */
        void Clear(std::string diagnostic = {});
        [[nodiscard]] uint64_t GetDiscardedCount() const;

        [[nodiscard]] const std::string& GetDebugName() const noexcept { return m_debugName; }

    private:
        struct Command final
        {
            std::function<void()> body;
            RenderCommandEvent completion;
        };

        mutable std::mutex m_mutex;
        std::deque<Command> m_commands;
        std::string m_debugName;
        uint64_t m_executedCount = 0u;
        uint64_t m_discardedCount = 0u;

        /**
         * Single-drainer ownership.
         *
         * FIFO is a property of the queue, not of execution: a command that has
         * been popped is no longer in the queue, so a second drainer could run a
         * later command while the first drainer is still running an earlier one.
         * A nested or competing drain is refused so the current command always
         * finishes before the next one starts. FlushRenderingCommands is a
         * game-thread operation and cannot re-enter from a render command.
         */
        mutable std::mutex m_drainMutex;
        bool m_draining = false;
    };

    /**
     * Sequence source the fence polls for the Rhi and Swapchain sync depths. The
     * RHI command list executor implements this.
     */
    class IRenderCommandSequenceSource
    {
    public:
        virtual ~IRenderCommandSequenceSource() = default;

        /** Work handed to the executor; may not be translated or submitted yet. */
        [[nodiscard]] virtual uint64_t GetDispatchedSequence() const = 0;
        /** Work already submitted to a GPU queue. */
        [[nodiscard]] virtual uint64_t GetSubmitSequence() const = 0;
        /** Presents already consumed by the swapchain. */
        [[nodiscard]] virtual uint64_t GetPresentSequence() const = 0;
        /** Last dispatched sequence that also requested a present. */
        [[nodiscard]] virtual uint64_t GetPresentRequestSequence() const = 0;
        /** Failure of a captured submission, excluding later unrelated work. */
        [[nodiscard]] virtual std::string GetSubmissionFailure(uint64_t sequence) const { return {}; }
    };

    /** Which stages of the RHI pipeline a flush has to reach. */
    enum class RhiFlushFlags : uint32_t
    {
        None = 0u,
        Dispatch = 1u << 0u,
        Translate = 1u << 1u,
        Submit = 1u << 2u,
        // Waiting for the GPU is never implicit: it must be requested explicitly.
        Gpu = 1u << 3u,
        Swapchain = 1u << 4u,
        ReleaseResources = 1u << 5u
    };

    inline constexpr RhiFlushFlags operator|(const RhiFlushFlags lhs, const RhiFlushFlags rhs)
    {
        return static_cast<RhiFlushFlags>(
            static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    [[nodiscard]] NLS_RENDER_API bool HasFlag(RhiFlushFlags value, RhiFlushFlags flag);

    /**
     * Global render command dispatcher.
     *
     * The main pipe is a strictly serial FIFO. The async pipe may run work that
     * does not touch render-thread state; its completion token must be merged into
     * the main pipe with MergeAsync before a main command consumes its output.
     */
    class NLS_RENDER_API RenderCommandDispatcher final
    {
    public:
        /**
         * How long a main-pipe merge waits for its async dependency.
         *
         * The wait is bounded because a dependency can be lost: async work is
         * submitted to a pool, and a pool that is shutting down cancels pending
         * tasks without running their bodies. Cancelled tasks release their tokens;
         * a producer outside the dispatcher may still violate that contract. A
         * timeout fails the pipe before any later command consumes its resources.
         */
        static constexpr std::chrono::milliseconds kDefaultAsyncMergeWait { 5000 };
        static void SetSequenceSource(IRenderCommandSequenceSource* source);
        [[nodiscard]] static IRenderCommandSequenceSource* GetSequenceSource();

        /**
         * Binds the pool that the async pipe executes on. Without a running graph
         * the async pipe degrades to inline execution, which keeps the API usable
         * in tests and in the no-threaded-rendering path.
         */
        static void SetTaskGraph(NLS::Base::TaskGraph::TaskGraph* taskGraph);
        [[nodiscard]] static NLS::Base::TaskGraph::TaskGraph* GetTaskGraph();

        /** Enqueue on the serial main pipe. Safe to call from any thread. */
        static void Enqueue(std::function<void()> command, RenderCommandEvent completion = {});

        /**
         * Enqueue independent work on the async pipe and return its dependency
         * token. With a bound TaskGraph the body runs on a pool worker in
         * parallel with the main pipe; MergeAsync is then the only correct way to
         * consume its output from the render thread.
         */
        static RenderCommandEvent EnqueueAsync(std::function<void()> command);

        /** Async bodies submitted but not finished yet. */
        [[nodiscard]] static size_t GetPendingAsyncCount();

        /**
         * Makes the main pipe wait for an async dependency before running the
         * commands enqueued after this call.
         */
        static void MergeAsync(RenderCommandEvent dependency);
        static void MergeAsync(
            RenderCommandEvent dependency,
            std::chrono::milliseconds maxWait);

        /** Async dependencies abandoned because they never completed. */
        [[nodiscard]] static uint64_t GetAbandonedAsyncMergeCount();

        /** Executes pending commands on the calling thread; returns the count. */
        static size_t Dispatch(size_t maxCommands = 0u);

        /** Dispatches until both pipes are empty; returns the total count. */
        static size_t FlushToDrain();

        [[nodiscard]] static bool IsEmpty();
        [[nodiscard]] static size_t GetMainQueueDepth();
        [[nodiscard]] static size_t GetAsyncQueueDepth();
        [[nodiscard]] static RenderCommandPipe& GetMainPipe();
        [[nodiscard]] static RenderCommandPipe& GetAsyncPipe();

        /** Releases queued command and async dependency waiters after a fatal error. */
        static void CancelPendingCommands(const std::string& diagnostic);

        /** Test hook: drops pending commands and clears the sequence source. */
        static void ResetForTesting();
    };

    /** Business commands enter the render thread through this serial pipe API. */
    template <typename Tag, typename Lambda>
    RenderCommandEvent EnqueueRenderCommand(Lambda&& command, RenderCommandEvent completion = {})
    {
        RenderCommandDispatcher::Enqueue(std::forward<Lambda>(command), completion);
        return completion;
    }

    /**
     * Hook the RHI executor registers so a flush can drive translate/submit/GPU
     * waits without this header depending on the executor.
     */
    using RenderCommandFlushHook = std::function<void(RhiFlushFlags)>;
    NLS_RENDER_API void SetRenderCommandFlushHook(RenderCommandFlushHook hook);

    /**
     * Game-thread only. Waits for render and RHI CPU work in flight. GPU waits
     * require Gpu; ReleaseResources only reclaims completed work, and Swapchain
     * waits for CPU Present handling.
     */
    NLS_RENDER_API void FlushRenderingCommands(
        RhiFlushFlags flags = RhiFlushFlags::Dispatch | RhiFlushFlags::Translate |
            RhiFlushFlags::Submit);
}
