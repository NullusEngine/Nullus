#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>

#include "RenderDef.h"
#include "Rendering/Context/RenderCommandPipe.h"

namespace NLS::Render::Context
{
    /**
     * Fence over the render command pipe.
     * UE equivalent: FRenderCommandFence.
     *
     * BeginFence enqueues a marker into the main pipe. When the marker runs it
     * captures the sequence numbers of the work submitted before it, so the fence
     * can wait for progressively deeper stages without waiting for unrelated work
     * submitted later:
     *
     *   Render    - the marker command has executed on the render thread.
     *   Rhi       - the submissions captured by the marker reached the GPU queue.
     *   Swapchain - the presents captured by the marker were consumed.
     */
    class NLS_RENDER_API RenderCommandFence final
    {
    public:
        RenderCommandFence() = default;

        /**
         * Starts the fence. Multiple BeginFence calls on the same object extend
         * the fence to the later sync point.
         */
        void BeginFence(RenderCommandSyncDepth depth = RenderCommandSyncDepth::Render);

        void Wait() const;
        /** True on any terminal outcome; inspect Succeeded/GetDiagnostic for failure. */
        [[nodiscard]] bool WaitFor(std::chrono::milliseconds timeout) const;
        [[nodiscard]] bool IsFenced() const;
        [[nodiscard]] bool Succeeded() const;
        [[nodiscard]] std::string GetDiagnostic() const;

        [[nodiscard]] bool IsValid() const noexcept { return m_marker.IsValid(); }

        /**
         * Completions that reported success at a shallower depth than requested
         * because nothing in this process publishes submission sequences.
         *
         * The Rhi and Swapchain depths are only observable when something
         * registers an IRenderCommandSequenceSource. Without one the fence still
         * completes - blocking forever would be worse, and callers poll it through
         * IsFenced - but it has not actually observed the depth it was asked for.
         * That is only acceptable while it stays visible: a non-zero count here
         * means a caller asked for RHI or present ordering this process cannot
         * report, so those flushes must not be trusted for RHI ordering.
         */
        [[nodiscard]] static uint64_t GetUnobservedDepthCompletionCount();
        static void ResetUnobservedDepthCompletionCountForTesting();

        [[nodiscard]] RenderCommandSyncDepth GetSyncDepth() const noexcept { return m_depth; }
        [[nodiscard]] uint64_t GetTargetSubmitSequence() const noexcept
        {
            return m_targets->submit.load(std::memory_order_acquire);
        }
        [[nodiscard]] uint64_t GetTargetPresentSequence() const noexcept
        {
            return m_targets->present.load(std::memory_order_acquire);
        }

    private:
        /**
         * Targets live in a separately owned block because the marker command runs
         * asynchronously on the render thread. Capturing `this` would let a fence
         * that is destroyed before its marker runs (for example after a WaitFor
         * timeout) be used after free.
         */
        struct TargetState final
        {
            std::atomic<uint64_t> submit { 0u };
            std::atomic<uint64_t> present { 0u };
            RenderCommandEvent outcome;
        };

        mutable RenderCommandEvent m_marker;
        RenderCommandSyncDepth m_depth = RenderCommandSyncDepth::Render;
        // Shared with the queued marker command; outlives this object if needed.
        std::shared_ptr<TargetState> m_targets { std::make_shared<TargetState>() };
    };
}
