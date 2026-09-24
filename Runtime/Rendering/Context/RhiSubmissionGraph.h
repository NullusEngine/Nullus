#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "RenderDef.h"
#include "TaskGraph/TaskGraph.h"

namespace NLS::Render::Context
{
    /**
     * Explicit RHI submission stages. Each stage is one TaskGraph node and the
     * stages are chained with TaskHandle prerequisites so the RHI named thread
     * never executes the pipeline itself; it only pumps its own queue.
     */
    enum class RhiSubmissionStage : uint8_t
    {
        RecordWorkUnits = 0u,
        TranslateWorkUnits,
        QueueSubmit,
        Present,
        Readback,
        Retire,
        Count
    };

    inline constexpr size_t kRhiSubmissionStageCount =
        static_cast<size_t>(RhiSubmissionStage::Count);

    /**
     * Stage bodies. A body signals an unrecoverable stage failure by throwing;
     * TaskGraph then fails that node and cancels every dependent node, which is
     * how cancellation reaches TaskCompletion.
     */
    struct RhiSubmissionStageCallbacks
    {
        std::function<void()> recordWorkUnits;
        std::function<void()> translateWorkUnits;
        std::function<void()> queueSubmit;
        std::function<void()> present;
        std::function<void()> readback;
        std::function<void()> retire;
    };

    struct RhiSubmissionGraphConfig
    {
        // Capability-limited combinations collapse to serial nodes owned by the
        // RHI named thread instead of falling back to any other scheduler.
        bool parallelRecordWorkUnits = false;
        bool parallelTranslateWorkUnits = false;
    };

    struct RhiSubmissionGraphSchedule
    {
        std::array<NLS::Base::TaskGraph::TaskHandle, kRhiSubmissionStageCount> stageHandles {};
        NLS::Base::TaskGraph::TaskCompletion completion;
        bool scheduled = false;
        bool inlineExecuted = false;

        [[nodiscard]] bool IsValid() const noexcept { return scheduled || inlineExecuted; }
        [[nodiscard]] NLS::Base::TaskGraph::TaskHandle GetStageHandle(
            RhiSubmissionStage stage) const noexcept;
    };

    class NLS_RENDER_API RhiSubmissionGraph final
    {
    public:
        /**
         * Submits the six stage nodes with an explicit dependency chain:
         * Record -> Translate -> QueueSubmit -> Present -> Readback -> Retire.
         * The returned completion belongs to the Retire node.
         */
        static RhiSubmissionGraphSchedule Schedule(
            NLS::Base::TaskGraph::TaskGraph& taskGraph,
            const RhiSubmissionGraphConfig& config,
            RhiSubmissionStageCallbacks callbacks);

        /**
         * Executes the same stage sequence inline on the calling thread. Used when
         * no TaskGraph executor exists so that threadless and deterministic paths
         * keep identical stage semantics.
         */
        static NLS::Base::TaskGraph::TaskStatus ExecuteInline(
            RhiSubmissionStageCallbacks& callbacks);

        /**
         * Waits for the Retire node while pumping the RHI named-thread queue, so a
         * synchronous drain caller can never deadlock on RHI-affine nodes.
         */
        static NLS::Base::TaskGraph::TaskStatus WaitForCompletion(
            NLS::Base::TaskGraph::TaskGraph& taskGraph,
            const RhiSubmissionGraphSchedule& schedule);
    };
}
