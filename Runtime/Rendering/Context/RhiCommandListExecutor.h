#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "RenderDef.h"
#include "Rendering/Context/RenderCommandPipe.h"
#include "Rendering/Context/RhiCommandList.h"
#include "Rendering/Context/RenderingThreadSystem.h"

namespace NLS::Base::TaskGraph
{
    class TaskGraph;
}

namespace NLS::Render::RHI
{
    class RHIDevice;
    class RHITexture;
    struct RHIReadbackResult;
}

namespace NLS::Render::Settings
{
    enum class EPixelDataFormat : uint8_t;
    enum class EPixelDataType : uint8_t;
}

namespace NLS::Render::Context
{
    class Driver;
    struct RenderScenePackage;
    enum class RhiSubmissionAttribution : uint8_t;
    namespace Detail
    {
        struct ThreadedRhiSubmissionExecution;
        struct RhiFrameExecutionQueue;
    }
    /**
     * Device-facing work the executor performs. Injecting these lets the pipeline
     * be tested without a GPU and keeps the executor independent of any backend.
     */
    struct NLS_RENDER_API RhiExecutorTargets
    {
        /** Acquires a command buffer for one batch. Runs on a translate worker. */
        std::function<std::shared_ptr<RHI::RHICommandBuffer>(RHI::QueueType, uint64_t sequence)>
            acquireCommandBuffer;
        /** Releases the translated buffer. Runs after the GPU fence has retired. */
        std::function<void(const RhiTranslatedBatch&)> retireCommandBuffer;
        /** Submits the translated batch. Runs on the RHI lane only. */
        std::function<RHI::RHIQueueOperationResult(const RhiTranslatedBatch&)> submit;
        /** Presents after a batch that requested it. Runs on the RHI lane only. */
        std::function<RHI::RHIQueueOperationResult(const RhiTranslatedBatch&)> present;
        /** Orders fresh retirement fences on every device queue, on the submit lane. */
        std::function<RHI::RHIQueueOperationResult(
            uint64_t, std::vector<std::shared_ptr<RHI::RHIFence>>&)> signalGpuFences;

        [[nodiscard]] bool IsValid() const
        {
            return static_cast<bool>(acquireCommandBuffer) && static_cast<bool>(submit);
        }
    };

    /** One ordered submission, translated as independent immutable lists. */
    struct NLS_RENDER_API RhiCommandListBatch
    {
        std::vector<std::shared_ptr<RhiCommandList>> commandLists;
        std::function<std::shared_ptr<RHI::RHICommandBuffer>(size_t, RHI::QueueType, uint64_t)>
            acquireCommandBuffer;
        std::function<RHI::RHIQueueOperationResult(const std::vector<RhiTranslatedBatch>&)> submit;
        std::function<RHI::RHIQueueOperationResult(const std::vector<RhiTranslatedBatch>&)> present;
        std::function<void(const RhiTranslatedBatch&)> retireCommandBuffer;
        // Wake the owner once CPU work and cancelled translations have stopped.
        std::function<void()> onComplete;
        // A custom submit/present callback signals this fence for the whole batch.
        std::shared_ptr<RHI::RHIFence> retirementFence;
        bool parallelTranslation = true;
        // A compiled queue plan commits transitions after each successful queue
        // submit, before its dependent readback or present operation.
        bool commitResourceStates = true;
        // Present may carry the retirement signal instead of the queue submit.
        // In that case only frameFenceSignalQueued confirms safe retirement.
        bool submitSignalsFences = true;
        bool requestsPresent = false;
        std::string debugName;
    };

    /**
     * Handle for one non-blocking submit-lane operation.
     *
     * EnqueueOnSubmitLane returns before the operation runs, so the caller can
     * keep preparing work while the lane drains the submissions queued ahead of
     * it. A rejected enqueue completes the handle immediately with the failure in
     * `result`; the operation never enters the lane.
     */
    struct NLS_RENDER_API RhiSubmitLaneOperation final
    {
        RhiSubmitEvent completion;
        std::shared_ptr<RHI::RHIQueueOperationResult> result =
            std::make_shared<RHI::RHIQueueOperationResult>();
    };

    /**
     * Global deferred RHI executor: dispatch -> parallel translate -> serialized
     * submit/present, in strict submission order.
     * UE equivalent: FRHICommandListExecutor with its dispatch, translate and
     * submit task pipes.
     *
     * Ordering is guaranteed by construction: only one submit may be in flight,
     * and the next sequence is only published after the previous submit returns.
     * Parallel translation may finish out of order; the ordered map plus the
     * single-flight submit lane restores FIFO submission.
     */
    class NLS_RENDER_API RhiCommandListExecutor final : public IRenderCommandSequenceSource
    {
    public:
        static RhiCommandListExecutor& Get();
        /** Binds all device queues to fresh retirement fences on the submit lane. */
        static RhiExecutorTargets CreateDeviceTargets(const std::shared_ptr<RHI::RHIDevice>& device);

        /**
         * Binds the executor to the rendering thread system. None mode may use
         * workers for translation and submits on the caller; DedicatedThread routes the RHI
         * lane to the dedicated thread; Tasks routes it to a high priority worker
         * while keeping the logical RHI lane scope.
         * cpuWaitTimeout bounds all pipeline waits, including validation-patched PSOs.
         */
        void Initialize(
            RhiThreadMode mode,
            RhiExecutorTargets targets,
            NLS::Base::TaskGraph::TaskGraph* taskGraph = nullptr,
            const void* owner = nullptr,
            std::chrono::milliseconds cpuWaitTimeout = std::chrono::seconds(10));
        /**
         * Tears the deferred pipeline down. A shutdown from a driver that no
         * longer owns the lane is refused: the lane is process-wide, so a
         * destroyed driver must not be able to strand the queue state a still
         * running driver depends on. Passing a null owner keeps the
         * unconditional behaviour used by shutdown paths without a driver.
         */
        void Shutdown(
            RhiFlushFlags flags = RhiFlushFlags::ReleaseResources,
            const void* owner = nullptr);
        [[nodiscard]] bool IsInitialized() const;
        void SetTargets(RhiExecutorTargets targets);

        /** Test/telemetry hook: which driver currently owns the submit lane. */
        [[nodiscard]] const void* GetOwner() const;

        /** Accepts a closed command list and assigns its submission sequence. */
        bool Dispatch(const std::shared_ptr<RhiCommandList>& commandList);

        /**
         * Assigns one sequence to a batch. Nothing from the batch reaches the GPU
         * until every list has translated successfully. Completion also waits for
         * cancelled translations to stop using their allocators.
         */
        bool DispatchBatch(RhiCommandListBatch batch, RhiSubmitEvent* completion = nullptr);

        /** Frame commands dispatch into this executor before later fence markers. */
        static bool DispatchRenderFrame(Driver& driver, size_t slotIndex, RenderScenePackage package);
        static bool TryScheduleIdleMeshRuntimeUploads(Driver& driver);
        /** Polls CPU completion without waiting for earlier frames. Runs on Render. */
        static bool FinalizeThreadedSubmissionProduction(Driver& driver, bool applyPendingSwapchainResize);
        /** Joins dispatched frame work before the owning Driver shuts down. */
        static void DrainProducerOwnedThreadedSubmission(Driver& driver);
        static void ReadPixels(
            const Driver& driver, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
            Settings::EPixelDataFormat format, Settings::EPixelDataType type, void* data);
        static void ReadPixels(
            const Driver& driver, const std::shared_ptr<RHI::RHITexture>& texture,
            uint32_t x, uint32_t y, uint32_t width, uint32_t height,
            Settings::EPixelDataFormat format, Settings::EPixelDataType type, void* data);
        static RHI::RHIReadbackResult ReadPixelsChecked(
            const Driver& driver, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
            Settings::EPixelDataFormat format, Settings::EPixelDataType type, void* data);
        static RHI::RHIReadbackResult ReadPixelsChecked(
            const Driver& driver, const std::shared_ptr<RHI::RHITexture>& texture,
            uint32_t x, uint32_t y, uint32_t width, uint32_t height,
            Settings::EPixelDataFormat format, Settings::EPixelDataType type, void* data);
        static RHI::RHIReadbackResult BeginReadPixels(
            const Driver& driver, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
            Settings::EPixelDataFormat format, Settings::EPixelDataType type, void* data);
        static RHI::RHIReadbackResult BeginReadPixels(
            const Driver& driver, const std::shared_ptr<RHI::RHITexture>& texture,
            uint32_t x, uint32_t y, uint32_t width, uint32_t height,
            Settings::EPixelDataFormat format, Settings::EPixelDataType type, void* data);
        static RHI::RHIReadbackResult PollReadbackCompletion(
            const Driver& driver, const RHI::RHIReadbackResult& readback);
        static void PresentSwapchain(Driver& driver);

        /** Backend operations inside an executor submit callback only. */
        RHI::RHIQueueOperationResult SubmitNative(
            const std::shared_ptr<RHI::RHIQueue>& queue, const RHI::RHISubmitDesc& desc);
        RHI::RHIQueueOperationResult PresentNative(
            const std::shared_ptr<RHI::RHIQueue>& queue, const RHI::RHIPresentDesc& desc);

        /**
         * Runs one backend queue operation (submit or present) on the serialized
         * submit lane. Every graphics/compute/copy submission and every present
         * must funnel through here: the executor owns the submission sequence and
         * the logical RHI lane identity, so a backend queue can never be reached
         * from an arbitrary worker or from the render thread directly.
         *
         * The operation enters the same FIFO as deferred lists. The caller waits
         * for its result while the configured RHI lane executes the action.
         * Failures cancel later operations; exceptions report unconfirmed GPU work
         * so the caller preserves the affected resources.
         */
        RHI::RHIQueueOperationResult RunOnSubmitLane(
            std::string debugName,
            const std::function<RHI::RHIQueueOperationResult()>& action,
            bool presents = false);

        /**
         * Enqueues one backend operation on the same strict FIFO as
         * RunOnSubmitLane and returns without waiting for it.
         *
         * Frame setup uses this to place swapchain acquisition in submission
         * order at frame begin while the frame continues preparing work; the
         * matching WaitForSubmitLaneOperation runs at the first point the frame
         * actually consumes the result. Ordering is unchanged: the operation
         * still executes after every submission queued before it.
         */
        RhiSubmitLaneOperation EnqueueOnSubmitLane(
            std::string debugName,
            std::function<RHI::RHIQueueOperationResult()> action,
            bool presents = false);

        /**
         * Blocks until a handle from EnqueueOnSubmitLane reaches a terminal
         * state and returns its result. Pumps the pipeline when the caller is
         * the only thread that can drive it, and reports a stuck lane as a
         * pipeline failure instead of hanging forever.
         */
        RHI::RHIQueueOperationResult WaitForSubmitLaneOperation(
            const RhiSubmitLaneOperation& operation,
            std::string debugName);

        /** Backend sub-operations share the active submission; external calls enter the FIFO. */
        RHI::RHIQueueOperationResult RunBackendOperation(
            std::string debugName,
            const std::function<RHI::RHIQueueOperationResult()>& action,
            bool presents = false);

        /**
         * Keeps owners alive through all earlier FIFO work, including native
         * operations. Returns immediately; only confirmed GPU completion releases
         * the owners. Failure or shutdown quarantines them for process lifetime.
         */
        RhiGpuFence DeferRelease(std::vector<std::shared_ptr<void>> resources);
        /** GPU completion point after all earlier FIFO work on every device queue. */
        RhiGpuFence InsertGpuFence();
        [[nodiscard]] size_t GetPendingResourceReleaseCount() const;
        [[nodiscard]] size_t GetQuarantinedResourceReleaseCount() const;

        /**
         * Drives one translate and one submit step on the calling thread. Used by
         * the RHI lane tick and by None mode.
         */
        bool Pump();

        /** Blocks until the requested pipeline stages have completed. */
        bool Flush(RhiFlushFlags flags);

        /** Retires translated batches whose GPU fence has completed. */
        size_t ReleaseRetiredBatches();

        [[nodiscard]] uint64_t GetDispatchedSequence() const override;
        [[nodiscard]] uint64_t GetSubmitSequence() const override;
        [[nodiscard]] uint64_t GetPresentSequence() const override;
        [[nodiscard]] uint64_t GetPresentRequestSequence() const override;
        [[nodiscard]] std::string GetSubmissionFailure(uint64_t sequence) const override;
        [[nodiscard]] uint64_t GetDispatchSequence() const;

        /**
         * True after an unrecoverable translate/submit failure. Outstanding waits
         * are released and further dispatch is rejected; callers must surface the
         * diagnostic and stop rendering.
         */
        [[nodiscard]] bool HasFailed() const;
        [[nodiscard]] std::string GetFailureDiagnostic() const;
        /** Cancels outstanding CPU work when an asynchronous token reports device loss. */
        void NotifyDeviceLost(std::string diagnostic);

        [[nodiscard]] size_t GetPendingTranslateCount() const;
        [[nodiscard]] size_t GetPendingSubmitCount() const;
        [[nodiscard]] bool IsSubmitInFlight() const;
        [[nodiscard]] RhiThreadMode GetMode() const;

        /** Test hook: clears queues, counters, failure state and sequence source. */
        void ResetForTesting();

    private:
        friend struct Detail::RhiFrameExecutionQueue;
        RhiCommandListExecutor() = default;
        ~RhiCommandListExecutor() override = default;

        mutable std::mutex m_frameMutex;
        std::mutex m_frameRetirementMutex;
        std::deque<std::shared_ptr<Detail::ThreadedRhiSubmissionExecution>> m_pendingFrames;

        /** Translates at most one dispatched list. Safe to call concurrently. */
        bool PumpTranslate();
        /** Submits at most one translated batch, in sequence order. */
        bool PumpSubmit();
        /** Schedules translate work on the pool, or runs it inline when unbound. */
        void ScheduleTranslateWork();
        /** Schedules the submit step on the RHI lane. */
        void ScheduleSubmitWork();
        /** Marks the pipeline failed and releases every outstanding waiter. */
        void FailPipeline(std::string diagnostic);
        RhiGpuFence QueueResourceRetirement(std::vector<std::shared_ptr<void>> resources);

        struct Impl;
        [[nodiscard]] Impl& State();
        [[nodiscard]] const Impl& State() const;
    };
}
