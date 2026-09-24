#include "Rendering/Context/RhiSubmissionGraph.h"

#include <chrono>
#include <utility>

namespace NLS::Render::Context
{
    namespace
    {
        using NLS::Base::TaskGraph::NamedThread;
        using NLS::Base::TaskGraph::TaskDesc;
        using NLS::Base::TaskGraph::TaskHandle;
        using NLS::Base::TaskGraph::TaskPriority;
        using NLS::Base::TaskGraph::TaskStatus;

        constexpr size_t ToStageIndex(const RhiSubmissionStage stage) noexcept
        {
            return static_cast<size_t>(stage);
        }

        constexpr const char* kParallelRecordNodeName = "RhiSubmissionGraph::RecordWorkUnits";
        constexpr const char* kSerialRecordNodeName = "RhiSubmissionGraph::RecordWorkUnits.Serial";
        constexpr const char* kParallelTranslateNodeName = "RhiSubmissionGraph::TranslateWorkUnits";
        constexpr const char* kSerialTranslateNodeName = "RhiSubmissionGraph::TranslateWorkUnits.Serial";
        constexpr const char* kQueueSubmitNodeName = "RhiSubmissionGraph::QueueSubmit";
        constexpr const char* kPresentNodeName = "RhiSubmissionGraph::Present";
        constexpr const char* kReadbackNodeName = "RhiSubmissionGraph::Readback";
        constexpr const char* kRetireNodeName = "RhiSubmissionGraph::Retire";
    }

    NLS::Base::TaskGraph::TaskHandle RhiSubmissionGraphSchedule::GetStageHandle(
        const RhiSubmissionStage stage) const noexcept
    {
        const size_t index = ToStageIndex(stage);
        if (index >= stageHandles.size())
            return {};
        return stageHandles[index];
    }

    RhiSubmissionGraphSchedule RhiSubmissionGraph::Schedule(
        NLS::Base::TaskGraph::TaskGraph& taskGraph,
        const RhiSubmissionGraphConfig& config,
        RhiSubmissionStageCallbacks callbacks)
    {
        RhiSubmissionGraphSchedule schedule;

        const auto submitStage = [&taskGraph, &schedule](
            const RhiSubmissionStage stage,
            const NamedThread targetThread,
            const char* const debugName,
            std::function<void()> body,
            const TaskHandle prerequisite)
        {
            TaskDesc desc;
            desc.function = std::move(body);
            if (prerequisite.IsValid())
                desc.prerequisites.push_back(prerequisite);
            desc.targetThread = targetThread;
            desc.priority = TaskPriority::High;
            desc.debugName = debugName;
            const TaskHandle handle = taskGraph.Submit(std::move(desc));
            schedule.stageHandles[ToStageIndex(stage)] = handle;
            return handle;
        };

        const NamedThread recordThread = config.parallelRecordWorkUnits
            ? NamedThread::Any
            : NamedThread::Rhi;
        const NamedThread translateThread = config.parallelTranslateWorkUnits
            ? NamedThread::Any
            : NamedThread::Rhi;

        const TaskHandle record = submitStage(
            RhiSubmissionStage::RecordWorkUnits,
            recordThread,
            config.parallelRecordWorkUnits ? kParallelRecordNodeName : kSerialRecordNodeName,
            std::move(callbacks.recordWorkUnits),
            {});
        const TaskHandle translate = submitStage(
            RhiSubmissionStage::TranslateWorkUnits,
            translateThread,
            config.parallelTranslateWorkUnits ? kParallelTranslateNodeName : kSerialTranslateNodeName,
            std::move(callbacks.translateWorkUnits),
            record);
        const TaskHandle queueSubmit = submitStage(
            RhiSubmissionStage::QueueSubmit,
            NamedThread::Rhi,
            kQueueSubmitNodeName,
            std::move(callbacks.queueSubmit),
            translate);
        const TaskHandle present = submitStage(
            RhiSubmissionStage::Present,
            NamedThread::Rhi,
            kPresentNodeName,
            std::move(callbacks.present),
            queueSubmit);
        const TaskHandle readback = submitStage(
            RhiSubmissionStage::Readback,
            NamedThread::Rhi,
            kReadbackNodeName,
            std::move(callbacks.readback),
            present);
        const TaskHandle retire = submitStage(
            RhiSubmissionStage::Retire,
            NamedThread::Rhi,
            kRetireNodeName,
            std::move(callbacks.retire),
            readback);

        schedule.completion = taskGraph.GetCompletion(retire);
        schedule.scheduled = retire.IsValid() && schedule.completion.IsValid();
        if (!schedule.scheduled)
            return schedule;

        // RHI-affine nodes are queued for the named thread, so wake it explicitly
        // instead of relying on the caller to poll.
        taskGraph.WakeNamedThread(NamedThread::Rhi);
        return schedule;
    }

    NLS::Base::TaskGraph::TaskStatus RhiSubmissionGraph::ExecuteInline(
        RhiSubmissionStageCallbacks& callbacks)
    {
        const auto runStage = [](const std::function<void()>& body) -> bool
        {
            if (!body)
                return true;
            try
            {
                body();
                return true;
            }
            catch (...)
            {
                return false;
            }
        };

        if (!runStage(callbacks.recordWorkUnits))
            return TaskStatus::Failed;
        if (!runStage(callbacks.translateWorkUnits))
            return TaskStatus::Failed;
        if (!runStage(callbacks.queueSubmit))
            return TaskStatus::Failed;
        if (!runStage(callbacks.present))
            return TaskStatus::Failed;
        if (!runStage(callbacks.readback))
            return TaskStatus::Failed;
        if (!runStage(callbacks.retire))
            return TaskStatus::Failed;
        return TaskStatus::Succeeded;
    }

    NLS::Base::TaskGraph::TaskStatus RhiSubmissionGraph::WaitForCompletion(
        NLS::Base::TaskGraph::TaskGraph& taskGraph,
        const RhiSubmissionGraphSchedule& schedule)
    {
        if (schedule.inlineExecuted)
            return TaskStatus::Succeeded;

        if (!schedule.completion.IsValid())
        {
            // A partial schedule can still own pending nodes; drain every handle we
            // did receive before releasing the payloads they captured.
            for (const TaskHandle handle : schedule.stageHandles)
            {
                if (handle.IsValid())
                    (void)taskGraph.Wait(handle);
            }
            (void)taskGraph.ReleaseTerminalTaskPayloads();
            return TaskStatus::Cancelled;
        }

        // The waiting thread pumps the RHI queue itself. On the RHI named thread
        // this is that thread's own pump; on a synchronous drain caller it keeps
        // the chain moving while the RHI worker thread is parked.
        auto completion = schedule.completion;
        while (!completion.IsComplete())
        {
            (void)taskGraph.PumpCurrentThread();
            (void)taskGraph.Pump(NamedThread::Rhi);
            if (completion.IsComplete())
                break;
            (void)completion.WaitFor(std::chrono::milliseconds(1));
        }

        // The graph keeps terminal nodes for handle and generation inspection, so
        // release their closures explicitly instead of pinning frame resources.
        (void)taskGraph.ReleaseTerminalTaskPayloads();
        return completion.GetStatus();
    }
}
