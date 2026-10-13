#include "Rendering/Context/RhiCommandListExecutor.h"

#include <stdexcept>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <unordered_set>
#include <vector>

#include <Debug/Logger.h>

#include "Profiling/Profiler.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "TaskGraph/TaskGraph.h"
#include "TaskGraph/TaskGraphSystem.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace NLS::Render::Context
{
    namespace
    {
        using NLS::Base::TaskGraph::NamedThread;
        using NLS::Base::TaskGraph::TaskGraph;
        using NLS::Base::TaskGraph::TaskPriority;

        constexpr auto kDefaultCpuWaitTimeout = std::chrono::seconds(10);

        /**
         * Logical single-flight guard for the submit lane. A plain flag cannot
         * detect nesting: the lane holds a mutex for the whole queue operation, so
         * a nested call from the same thread would deadlock instead of reporting.
         */
        bool& SubmitLaneReenteredFlag()
        {
            static thread_local bool reentered = false;
            return reentered;
        }

#if defined(NLS_ENABLE_TEST_HOOKS)
        // Set by SetRecordObserverForTesting. Read on the recording thread only.
        RhiCommandList::RecordObserverForTesting s_recordObserverForTesting;
#endif
        class ScopedSubmitLaneFlag final
        {
        public:
            ScopedSubmitLaneFlag() { SubmitLaneReenteredFlag() = true; }
            ~ScopedSubmitLaneFlag() { SubmitLaneReenteredFlag() = false; }

            ScopedSubmitLaneFlag(const ScopedSubmitLaneFlag&) = delete;
            ScopedSubmitLaneFlag& operator=(const ScopedSubmitLaneFlag&) = delete;
        };

        std::string CurrentExceptionDiagnostic(const std::string& context)
        {
            try
            {
                throw;
            }
            catch (const std::exception& error)
            {
                return context + ": " + error.what();
            }
            catch (...)
            {
                return context + ": unknown exception";
            }
        }
    }

    struct RhiCommandListExecutor::Impl final
    {
        struct DispatchedBatch final
        {
            uint64_t sequence = 0u;
            RhiCommandListBatch request;
            std::vector<RhiTranslatedBatch> translated;
            size_t remainingTranslations = 0u;
            size_t activeTranslations = 0u;
            RhiSubmitEvent completion;
        };

        struct TranslateWork final
        {
            std::shared_ptr<DispatchedBatch> batch;
            size_t listIndex = 0u;
        };

        struct RetiredBatch final
        {
            RhiTranslatedBatch translated;
            std::shared_ptr<RHI::RHIFence> retirementFence;
            std::function<void(const RhiTranslatedBatch&)> retire;
        };

        struct NativeSubmission final
        {
            uint64_t sequence = 0u;
            std::string debugName;
            std::function<RHI::RHIQueueOperationResult()> action;
            bool presents = false;
            // Shared with the handle returned by EnqueueOnSubmitLane so a caller
            // that waited later observes exactly the lane's result.
            std::shared_ptr<RHI::RHIQueueOperationResult> result =
                std::make_shared<RHI::RHIQueueOperationResult>();
            RhiSubmitEvent completion;
        };

        struct RetiringResources final
        {
            RhiGpuFence completion;
            std::vector<std::shared_ptr<void>> owners;
            std::vector<std::shared_ptr<RHI::RHIFence>> fences;
            bool submitted = false;
        };

        mutable std::mutex mutex;
        std::condition_variable stateChanged;

        RhiThreadMode mode = RhiThreadMode::DedicatedThread;
        std::chrono::milliseconds cpuWaitTimeout { kDefaultCpuWaitTimeout };
        RhiExecutorTargets targets;
        TaskGraph* taskGraph = nullptr;
        // Driver that currently owns the process-wide lane. The engine is
        // single-active-driver by contract, but a second Driver can still be
        // constructed before the first is destroyed, so ownership is explicit
        // rather than implied.
        const void* owner = nullptr;
        bool initialized = false;
        // Startup operations use the same inline FIFO until threads are bound.
        bool acceptingNative = true;

        // Dispatch pipe: closed lists waiting for translation, in arrival order.
        std::deque<TranslateWork> pendingTranslate;
        uint64_t dispatchSequence = 0u;
        size_t translateInFlight = 0u;
        size_t maxTranslateInFlight = 1u;
        bool serialTranslationInFlight = false;
        std::map<uint64_t, std::shared_ptr<DispatchedBatch>> dispatches;

        // Translate results keyed by sequence so out-of-order completion cannot
        // change the submission order.
        std::map<uint64_t, std::shared_ptr<DispatchedBatch>> translated;
        // Highest sequence whose translation has finished. Tracked separately from
        // the map because submitted entries leave the map, and translate completion
        // must not be mistaken for submit completion.
        uint64_t lastTranslatedSequence = 0u;
        uint64_t nextSubmitSequence = 1u;
        uint64_t lastPresentRequestedSequence = 0u;
        bool submitInFlight = false;

        uint64_t submitSequence = 0u;
        uint64_t presentSequence = 0u;
        uint64_t lastGpuFenceSequence = 0u;
        uint64_t gpuCompletedSequence = 0u;

        // Already-translated operations occupy the same sequence space and
        // submit FIFO as deferred command lists.
        std::map<uint64_t, std::shared_ptr<NativeSubmission>> nativeSubmissions;
        // Serializes whole queue operations, not just the bookkeeping around them.
        std::mutex submitLaneMutex;

        // Submitted batches awaiting GPU completion before their resources may be
        // reset and reused.
        std::deque<RetiredBatch> retireQueue;
        // Shutdown without confirmed GPU completion cannot destroy these owners.
        std::deque<RetiredBatch> quarantinedBatches;
        std::deque<std::shared_ptr<RetiringResources>> retiringResources;
        std::deque<std::shared_ptr<RetiringResources>> quarantinedResources;
        std::deque<RhiExecutorTargets> quarantinedTargets;
        size_t resourceRetireInFlight = 0u;

        bool failed = false;
        uint64_t firstFailedSequence = 0u;
        std::string failureDiagnostic;

        void MarkFailedLocked(const std::string& diagnostic)
        {
            if (failed)
                return;
            firstFailedSequence = nextSubmitSequence;
            failureDiagnostic = diagnostic;
            failed = true;
        }

        [[nodiscard]] bool IsIdleLocked() const
        {
            return pendingTranslate.empty() &&
                translated.empty() &&
                nativeSubmissions.empty() &&
                !submitInFlight &&
                nextSubmitSequence > dispatchSequence;
        }

        static void SignalFailedLists(const DispatchedBatch& batch, const std::string& diagnostic)
        {
            for (const auto& list : batch.request.commandLists)
            {
                list->GetTranslateEvent().Signal(false, diagnostic);
                list->GetSubmitEvent().Signal(false, diagnostic);
                list->GetGpuFence().Signal(false, diagnostic);
            }
        }

        static void ReleaseCancelledBatch(DispatchedBatch& batch, const std::string& diagnostic)
        {
            SignalFailedLists(batch, diagnostic);
            for (const auto& result : batch.translated)
            {
                if (result.commandBuffer != nullptr && batch.request.retireCommandBuffer)
                {
                    try
                    {
                        batch.request.retireCommandBuffer(result);
                    }
                    catch (...)
                    {
                        NLS_LOG_ERROR(CurrentExceptionDiagnostic("Cancelled RHI batch retirement"));
                    }
                }
            }
            SignalCompletion(batch, false, diagnostic);
        }

        static void SignalCompletion(DispatchedBatch& batch, const bool success, const std::string& diagnostic = {})
        {
            auto notification = std::move(batch.request.onComplete);
            // The owner may resize or tear down its device as soon as completion
            // is visible. GPU owners moved to retireQueue; drop CPU frame captures.
            batch.request.commandLists.clear();
            batch.request.acquireCommandBuffer = {};
            batch.request.submit = {};
            batch.request.present = {};
            batch.request.retireCommandBuffer = {};
            batch.completion.Signal(success, diagnostic, [&notification]
            {
                if (!notification)
                    return;
                try
                {
                    notification();
                }
                catch (...)
                {
                    NLS_LOG_ERROR(CurrentExceptionDiagnostic("RHI completion notification"));
                }
                notification = {};
            });
        }
    };

    RhiCommandListExecutor::Impl& RhiCommandListExecutor::State()
    {
        // Process-lifetime so late render commands cannot touch a destroyed lane.
        static Impl* impl = new Impl();
        return *impl;
    }

    const RhiCommandListExecutor::Impl& RhiCommandListExecutor::State() const
    {
        return const_cast<RhiCommandListExecutor*>(this)->State();
    }

    RhiCommandListExecutor& RhiCommandListExecutor::Get()
    {
        static RhiCommandListExecutor* instance = new RhiCommandListExecutor();
        return *instance;
    }

    RhiExecutorTargets RhiCommandListExecutor::CreateDeviceTargets(const std::shared_ptr<RHI::RHIDevice>& device)
    {
        RhiExecutorTargets targets;
        if (device == nullptr)
            return targets;
        targets.signalGpuFences = [device](const uint64_t sequence,
            std::vector<std::shared_ptr<RHI::RHIFence>>& fences)
        {
            std::vector<std::shared_ptr<RHI::RHIQueue>> queues;
            for (const auto type : {RHI::QueueType::Copy, RHI::QueueType::Compute, RHI::QueueType::Graphics})
            {
                auto queue = device->GetQueue(type);
                if (queue == nullptr || std::find(queues.begin(), queues.end(), queue) != queues.end())
                    continue;
                queues.push_back(queue);
                auto fence = device->CreateFence("ResourceRetirement" + std::to_string(sequence) +
                    "Queue" + std::to_string(static_cast<uint32_t>(type)));
                if (fence == nullptr)
                    return RHI::RHIQueueOperationResult{
                        RHI::RHIQueueOperationStatusCode::BackendFailure,
                        "GPU resource retirement could not create a queue fence", true};
                fence->Reset();
                fences.push_back(fence);
                RHI::RHISubmitDesc desc;
                desc.signalFence = std::move(fence);
                auto result = RhiCommandListExecutor::Get().SubmitNative(queue, desc);
                if (!result.Succeeded() || !result.frameFenceSignalQueued)
                {
                    if (result.Succeeded())
                    {
                        result.code = RHI::RHIQueueOperationStatusCode::BackendFailure;
                        result.message = "GPU resource retirement fence was not queued";
                    }
                    result.mayHaveQueuedGpuWork = true;
                    return result;
                }
            }
            return RHI::RHIQueueOperationResult{
                RHI::RHIQueueOperationStatusCode::Success, {}, true, !fences.empty()};
        };
        return targets;
    }

    void RhiCommandListExecutor::Initialize(
        const RhiThreadMode mode,
        RhiExecutorTargets targets,
        TaskGraph* taskGraph,
        const void* owner,
        const std::chrono::milliseconds cpuWaitTimeout)
    {
        if (cpuWaitTimeout <= std::chrono::milliseconds::zero())
            throw std::invalid_argument("RHI CPU wait timeout must be positive");
        Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        if (impl.owner != nullptr && owner != nullptr && impl.owner != owner && impl.initialized)
        {
            // Two live drivers sharing one lane would let either of them tear
            // down the other's queue state. Surface the ordering problem instead
            // of silently handing the lane over.
            NLS_LOG_ERROR(
                "RhiCommandListExecutor: submit lane is being taken over while another "
                "driver still owns it; the previous driver's submissions will be refused.");
        }
        impl.owner = owner;
        impl.mode = mode;
        impl.cpuWaitTimeout = cpuWaitTimeout;
        impl.targets = std::move(targets);
        impl.taskGraph = taskGraph;
        impl.initialized = true;
        impl.acceptingNative = true;
        impl.failed = false;
        impl.firstFailedSequence = 0u;
        impl.failureDiagnostic.clear();
        // Bound translation without reducing a two-worker pool to one: a blocked
        // earlier list must still permit independent later lists to translate.
        const uint32_t workerCount = taskGraph != nullptr ? taskGraph->GetWorkerCount() : 1u;
        impl.maxTranslateInFlight =
            (std::min)(static_cast<size_t>(4u), (std::max)(static_cast<size_t>(1u),
                static_cast<size_t>(workerCount)));
        RenderCommandDispatcher::SetSequenceSource(&Get());
    }

    void RhiCommandListExecutor::Shutdown(const RhiFlushFlags flags, const void* owner)
    {
        Impl& impl = State();
        {
            std::lock_guard lock(impl.mutex);
            if (owner != nullptr && impl.owner != nullptr && impl.owner != owner)
            {
                // A driver that lost the lane must not clear queue state the new
                // owner is already using.
                return;
            }
        }

        (void)Flush(flags | RhiFlushFlags::Dispatch | RhiFlushFlags::Translate | RhiFlushFlags::Submit);
        (void)ReleaseRetiredBatches();
        std::unique_lock laneLock(impl.submitLaneMutex);

        std::vector<std::shared_ptr<Impl::DispatchedBatch>> abandoned;
        std::map<uint64_t, std::shared_ptr<Impl::NativeSubmission>> abandonedNative;
        RhiExecutorTargets releasedTargets;
        {
            std::lock_guard lock(impl.mutex);
            if (owner != nullptr && impl.owner != nullptr && impl.owner != owner)
                return;
            impl.initialized = false;
            impl.acceptingNative = false;
            releasedTargets = std::move(impl.targets);
            // A moved-from std::function is only "valid but unspecified", and libc++
            // may keep a trivially copyable target alive. The member must not retain
            // device owners after shutdown.
            impl.targets = {};
            if (releasedTargets.signalGpuFences &&
                (impl.failed || impl.gpuCompletedSequence < impl.dispatchSequence))
                impl.quarantinedTargets.push_back(std::move(releasedTargets));
            if (owner != nullptr)
                impl.owner = nullptr;
            impl.pendingTranslate.clear();
            impl.translated.clear();
            abandonedNative.swap(impl.nativeSubmissions);
            for (auto it = impl.dispatches.begin(); it != impl.dispatches.end();)
            {
                Impl::SignalFailedLists(*it->second, "RHI executor shut down before completion");
                if (it->second->activeTranslations == 0u)
                {
                    abandoned.push_back(it->second);
                    it = impl.dispatches.erase(it);
                }
                else
                    ++it;
            }
            // Every dispatched sequence is accounted for, so a flush waiting on
            // translation is released instead of hanging on a dead lane.
            impl.lastTranslatedSequence = impl.dispatchSequence;
            impl.submitInFlight = false;
            while (!impl.retireQueue.empty())
            {
                impl.retireQueue.front().translated.gpuEvent.Signal(false, "RHI executor shut down before GPU completion");
                impl.quarantinedBatches.push_back(std::move(impl.retireQueue.front()));
                impl.retireQueue.pop_front();
            }
            while (!impl.retiringResources.empty())
            {
                auto resources = std::move(impl.retiringResources.front());
                resources->completion.Signal(false, "RHI executor shut down before resource retirement");
                impl.quarantinedResources.push_back(std::move(resources));
                impl.retiringResources.pop_front();
            }
        }
        laneLock.unlock();

        const std::string diagnostic = "RHI executor shut down before completion";
        for (auto& [sequence, operation] : abandonedNative)
        {
            (void)sequence;
            operation->result->code = RHI::RHIQueueOperationStatusCode::BackendFailure;
            operation->result->message = diagnostic;
            operation->action = {};
            operation->completion.Signal(false, diagnostic);
        }
        for (const auto& batch : abandoned)
            Impl::ReleaseCancelledBatch(*batch, diagnostic);
        impl.stateChanged.notify_all();

        if (RenderCommandDispatcher::GetSequenceSource() == this)
            RenderCommandDispatcher::SetSequenceSource(nullptr);
    }

    bool RhiCommandListExecutor::IsInitialized() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.initialized;
    }

    const void* RhiCommandListExecutor::GetOwner() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.owner;
    }

    void RhiCommandListExecutor::SetTargets(RhiExecutorTargets targets)
    {
        Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        impl.targets = std::move(targets);
    }

    bool RhiCommandListExecutor::Dispatch(const std::shared_ptr<RhiCommandList>& commandList)
    {
        if (commandList == nullptr)
            return false;

        RhiExecutorTargets targets;
        {
            std::lock_guard lock(State().mutex);
            targets = State().targets;
        }
        RhiCommandListBatch batch;
        batch.commandLists.push_back(commandList);
        batch.debugName = commandList->GetDebugName();
        batch.requestsPresent = commandList->RequestsPresent();
        if (targets.acquireCommandBuffer)
            batch.acquireCommandBuffer = [acquire = std::move(targets.acquireCommandBuffer)](
                size_t, const RHI::QueueType queue, const uint64_t sequence)
            { return acquire(queue, sequence); };
        if (targets.submit)
            batch.submit = [submit = std::move(targets.submit)](const auto& translated)
            { return submit(translated.front()); };
        if (targets.present)
            batch.present = [present = std::move(targets.present)](const auto& translated)
            { return present(translated.front()); };
        batch.retireCommandBuffer = std::move(targets.retireCommandBuffer);
        return DispatchBatch(std::move(batch));
    }

    bool RhiCommandListExecutor::DispatchBatch(
        RhiCommandListBatch request, RhiSubmitEvent* completion)
    {
        auto batch = std::make_shared<Impl::DispatchedBatch>();
        batch->request = std::move(request);
        batch->translated.resize(batch->request.commandLists.size());
        batch->remainingTranslations = batch->request.commandLists.size();
        if (completion != nullptr)
            *completion = batch->completion;

        Impl& impl = State();
        {
            std::lock_guard lock(impl.mutex);
            std::unordered_set<const RhiCommandList*> uniqueLists;
            const bool listsValid = std::all_of(
                batch->request.commandLists.begin(), batch->request.commandLists.end(),
                [&uniqueLists](const auto& list)
                { return list != nullptr && list->IsClosed() && uniqueLists.insert(list.get()).second; });
            if (!impl.initialized || impl.failed || !listsValid)
            {
                const std::string diagnostic = impl.failed ? impl.failureDiagnostic
                    : (listsValid ? "RHI executor is not ready" : "RHI batch requires closed command lists");
                for (const auto& list : batch->request.commandLists)
                {
                    if (list == nullptr)
                        continue;
                    list->GetDispatchEvent().Signal(false, diagnostic);
                    list->GetTranslateEvent().Signal(false, diagnostic);
                    list->GetSubmitEvent().Signal(false, diagnostic);
                    list->GetGpuFence().Signal(false, diagnostic);
                }
                batch->completion.Signal(false, diagnostic);
                return false;
            }

            const uint64_t sequence = ++impl.dispatchSequence;
            batch->sequence = sequence;
            batch->completion.SetSequence(sequence);
            impl.dispatches.emplace(sequence, batch);
            for (size_t index = 0u; index < batch->request.commandLists.size(); ++index)
            {
                const auto& list = batch->request.commandLists[index];
                list->m_sequence = sequence;
                list->m_state = RhiCommandList::State::Submitted;
                list->GetDispatchEvent().SetSequence(sequence);
                list->GetTranslateEvent().SetSequence(sequence);
                list->GetSubmitEvent().SetSequence(sequence);
                list->GetGpuFence().SetSequence(sequence);
                list->GetDispatchEvent().Signal(true);
                impl.pendingTranslate.push_back({batch, index});
            }
            if (batch->request.requestsPresent)
                impl.lastPresentRequestedSequence = sequence;
            if (batch->remainingTranslations == 0u)
            {
                impl.translated.emplace(sequence, batch);
                impl.lastTranslatedSequence = (std::max)(impl.lastTranslatedSequence, sequence);
            }
        }

        impl.stateChanged.notify_all();
        ScheduleTranslateWork();
        ScheduleSubmitWork();
        TaskGraph* graph = nullptr;
        RhiThreadMode mode;
        std::chrono::milliseconds cpuWaitTimeout;
        {
            std::lock_guard lock(impl.mutex);
            graph = impl.taskGraph;
            mode = impl.mode;
            cpuWaitTimeout = impl.cpuWaitTimeout;
        }
        if (mode == RhiThreadMode::None && graph != nullptr && graph->IsStarted())
        {
            const auto deadline = std::chrono::steady_clock::now() + cpuWaitTimeout;
            bool timedOut = false;
            while (!batch->completion.IsComplete())
            {
                if (graph->IsWorkerThread())
                    (void)graph->PumpCurrentThread(1u);
                (void)PumpSubmit();
                if (!timedOut && std::chrono::steady_clock::now() >= deadline)
                {
                    timedOut = true;
                    FailPipeline("RHI batch translation timed out: " + batch->request.debugName);
                }
                (void)batch->completion.WaitFor(std::chrono::milliseconds(1));
            }
        }
        return true;
    }

    RHI::RHIQueueOperationResult RhiCommandListExecutor::SubmitNative(
        const std::shared_ptr<RHI::RHIQueue>& queue, const RHI::RHISubmitDesc& desc)
    {
        if (!SubmitLaneReenteredFlag() || !RenderingThreadSystem::IsInRhiThread() || queue == nullptr)
            return {RHI::RHIQueueOperationStatusCode::InvalidArgument,
                "Native queue submission requires an executor submit callback and a queue"};
        if (HasFailed())
            return {RHI::RHIQueueOperationStatusCode::BackendFailure, GetFailureDiagnostic()};
        try
        {
            return queue->SubmitChecked(desc);
        }
        catch (...)
        {
            RHI::RHIQueueOperationResult result;
            result.code = RHI::RHIQueueOperationStatusCode::BackendFailure;
            result.message = CurrentExceptionDiagnostic("Native queue submit");
            result.mayHaveQueuedGpuWork = true;
            return result;
        }
    }

    RHI::RHIQueueOperationResult RhiCommandListExecutor::PresentNative(
        const std::shared_ptr<RHI::RHIQueue>& queue, const RHI::RHIPresentDesc& desc)
    {
        if (!SubmitLaneReenteredFlag() || !RenderingThreadSystem::IsInRhiThread() || queue == nullptr)
            return {RHI::RHIQueueOperationStatusCode::InvalidArgument,
                "Native present requires an executor submit callback and a queue"};
        if (HasFailed())
            return {RHI::RHIQueueOperationStatusCode::BackendFailure, GetFailureDiagnostic()};
        try
        {
            return queue->PresentChecked(desc);
        }
        catch (...)
        {
            RHI::RHIQueueOperationResult result;
            result.code = RHI::RHIQueueOperationStatusCode::BackendFailure;
            result.message = CurrentExceptionDiagnostic("Native queue present");
            result.mayHaveQueuedGpuWork = true;
            return result;
        }
    }

    RhiSubmitLaneOperation RhiCommandListExecutor::EnqueueOnSubmitLane(
        std::string debugName,
        std::function<RHI::RHIQueueOperationResult()> action,
        const bool presents)
    {
        RhiSubmitLaneOperation handle;
        if (!action)
        {
            handle.result->code = RHI::RHIQueueOperationStatusCode::InvalidArgument;
            handle.result->message = "RHI submit lane received an empty action";
            handle.completion.Signal(false, handle.result->message);
            return handle;
        }

        Impl& impl = State();
        if (SubmitLaneReenteredFlag())
        {
            // Re-entering from inside a submit callback would violate the backend
            // queue contract, so it is rejected rather than serialized by luck.
            handle.result->code = RHI::RHIQueueOperationStatusCode::BackendFailure;
            handle.result->message =
                "RHI submit lane re-entered from inside a queue operation";
            handle.completion.Signal(false, handle.result->message);
            return handle;
        }

        auto operation = std::make_shared<Impl::NativeSubmission>();
        operation->debugName = std::move(debugName);
        operation->action = std::move(action);
        operation->presents = presents;
        operation->result = handle.result;
        handle.completion = operation->completion;
        {
            std::lock_guard lock(impl.mutex);
            // A quarantined pipeline must not touch the device again: report the
            // same failure to every caller instead of silently resuming.
            if (impl.failed || !impl.acceptingNative)
            {
                handle.result->code = RHI::RHIQueueOperationStatusCode::BackendFailure;
                handle.result->message = impl.failed
                    ? "RHI submit lane is failed: " + impl.failureDiagnostic
                    : "RHI executor is shut down";
                handle.completion.Signal(false, handle.result->message);
                return handle;
            }

            operation->sequence = ++impl.dispatchSequence;
            operation->completion.SetSequence(operation->sequence);
            handle.completion.SetSequence(operation->sequence);
            impl.lastTranslatedSequence =
                (std::max)(impl.lastTranslatedSequence, operation->sequence);
            if (presents)
                impl.lastPresentRequestedSequence = operation->sequence;
            impl.nativeSubmissions.emplace(operation->sequence, operation);
        }
        impl.stateChanged.notify_all();
        ScheduleSubmitWork();
        return handle;
    }

    RHI::RHIQueueOperationResult RhiCommandListExecutor::WaitForSubmitLaneOperation(
        const RhiSubmitLaneOperation& operation,
        std::string debugName)
    {
        if (operation.completion.IsComplete())
            return *operation.result;

        Impl& impl = State();
        TaskGraph* graph = nullptr;
        RhiThreadMode mode = RhiThreadMode::None;
        std::chrono::milliseconds cpuWaitTimeout;
        {
            std::lock_guard lock(impl.mutex);
            graph = impl.taskGraph;
            mode = impl.mode;
            cpuWaitTimeout = impl.cpuWaitTimeout;
        }

        const auto deadline = std::chrono::steady_clock::now() + cpuWaitTimeout;
        bool reportedTimeout = false;
        while (!operation.completion.IsComplete())
        {
            if (mode == RhiThreadMode::None || graph == nullptr || !graph->IsStarted())
            {
                (void)Pump();
            }
            else if (graph->IsWorkerThread() || graph->GetCurrentNamedThread() == NamedThread::Rhi)
            {
                // Native callers can themselves be pool workers. Assisting the
                // graph keeps earlier translate work runnable with a single worker.
                (void)graph->PumpCurrentThread(1u);
            }
            if (!reportedTimeout && std::chrono::steady_clock::now() >= deadline)
            {
                reportedTimeout = true;
                FailPipeline("RHI native submit timed out: " + debugName);
            }
            (void)operation.completion.WaitFor(std::chrono::milliseconds(1));
        }
        return *operation.result;
    }

    RHI::RHIQueueOperationResult RhiCommandListExecutor::RunOnSubmitLane(
        std::string debugName,
        const std::function<RHI::RHIQueueOperationResult()>& action,
        const bool presents)
    {
        const RhiSubmitLaneOperation operation = EnqueueOnSubmitLane(debugName, action, presents);
        return WaitForSubmitLaneOperation(operation, std::move(debugName));
    }

    RHI::RHIQueueOperationResult RhiCommandListExecutor::RunBackendOperation(
        std::string debugName,
        const std::function<RHI::RHIQueueOperationResult()>& action,
        const bool presents)
    {
        if (!SubmitLaneReenteredFlag())
            return RunOnSubmitLane(std::move(debugName), action, presents);

        RHI::RHIQueueOperationResult result;
        if (!action)
        {
            result.code = RHI::RHIQueueOperationStatusCode::InvalidArgument;
            result.message = "RHI backend operation received an empty action";
            return result;
        }
        if (HasFailed())
        {
            result.code = RHI::RHIQueueOperationStatusCode::BackendFailure;
            result.message = GetFailureDiagnostic();
            return result;
        }

        try
        {
            result = action();
        }
        catch (...)
        {
            result.code = RHI::RHIQueueOperationStatusCode::BackendFailure;
            result.message = CurrentExceptionDiagnostic(debugName);
            result.mayHaveQueuedGpuWork = true;
        }
        // An out-of-date swapchain is a recoverable present outcome, not a pipeline
        // failure: the frame retires through its fence and the caller rebuilds the
        // swapchain. Only a real backend failure poisons the executor here.
        if (!result.Succeeded() && !(presents && result.RequiresSwapchainRecreation()))
            FailPipeline(result.message.empty() ? debugName + " failed" : result.message);
        return result;
    }

    RhiGpuFence RhiCommandListExecutor::DeferRelease(std::vector<std::shared_ptr<void>> resources)
    {
        resources.erase(std::remove(resources.begin(), resources.end(), nullptr), resources.end());
        if (resources.empty())
        {
            RhiGpuFence completed;
            completed.Signal(true);
            return completed;
        }
        return QueueResourceRetirement(std::move(resources));
    }

    RhiGpuFence RhiCommandListExecutor::InsertGpuFence()
    {
        return QueueResourceRetirement({});
    }

    RhiGpuFence RhiCommandListExecutor::QueueResourceRetirement(std::vector<std::shared_ptr<void>> resources)
    {
        auto retirement = std::make_shared<Impl::RetiringResources>();
        retirement->owners = std::move(resources);
        Impl& impl = State();
        auto operation = std::make_shared<Impl::NativeSubmission>();
        operation->debugName = "RhiCommandListExecutor::RetireResources";
        RhiThreadMode mode;
        {
            std::unique_lock lock(impl.mutex);
            if (!impl.failed && impl.gpuCompletedSequence >= impl.dispatchSequence)
            {
                // Also permits late scene destruction after a confirmed GPU shutdown.
                retirement->completion.Signal(true);
                return retirement->completion;
            }
            if (impl.failed || !impl.acceptingNative || !impl.targets.signalGpuFences)
            {
                const bool missingTarget = !impl.failed && impl.acceptingNative;
                const std::string diagnostic = impl.failed ? impl.failureDiagnostic :
                    "RHI resource retirement has no active GPU fence target";
                retirement->completion.Signal(false, diagnostic);
                impl.quarantinedResources.push_back(retirement);
                lock.unlock();
                if (missingTarget)
                    FailPipeline(diagnostic);
                return retirement->completion;
            }

            operation->sequence = ++impl.dispatchSequence;
            impl.lastGpuFenceSequence = operation->sequence;
            operation->completion.SetSequence(operation->sequence);
            retirement->completion.SetSequence(operation->sequence);
            operation->action = [this, retirement, signal = impl.targets.signalGpuFences,
                sequence = operation->sequence]
            {
                auto result = signal(sequence, retirement->fences);
                if (result.Succeeded() && (retirement->fences.empty() ||
                    std::any_of(retirement->fences.begin(), retirement->fences.end(),
                        [](const auto& fence) { return fence == nullptr; }) ||
                    !result.frameFenceSignalQueued))
                {
                    result.code = RHI::RHIQueueOperationStatusCode::BackendFailure;
                    result.message = "RHI resource retirement did not queue every GPU fence";
                }
                if (!result.Succeeded() && result.message.empty())
                    result.message = "GPU resource retirement signal failed";
                {
                    std::lock_guard retirementLock(State().mutex);
                    retirement->submitted = result.Succeeded();
                    if (!result.Succeeded())
                        retirement->completion.Signal(false, result.message);
                }
                return result;
            };
            impl.lastTranslatedSequence = (std::max)(impl.lastTranslatedSequence, operation->sequence);
            impl.retiringResources.push_back(retirement);
            impl.nativeSubmissions.emplace(operation->sequence, operation);
            mode = impl.mode;
        }
        impl.stateChanged.notify_all();
        ScheduleSubmitWork();
        if (mode == RhiThreadMode::None)
        {
            while (Pump())
            {
            }
        }
        return retirement->completion;
    }

    size_t RhiCommandListExecutor::GetPendingResourceReleaseCount() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.retiringResources.size() + impl.resourceRetireInFlight;
    }

    size_t RhiCommandListExecutor::GetQuarantinedResourceReleaseCount() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.quarantinedResources.size();
    }

    uint64_t RhiCommandListExecutor::GetDispatchSequence() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.dispatchSequence;
    }

    uint64_t RhiCommandListExecutor::GetDispatchedSequence() const
    {
        return GetDispatchSequence();
    }

    uint64_t RhiCommandListExecutor::GetSubmitSequence() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.submitSequence;
    }

    uint64_t RhiCommandListExecutor::GetPresentSequence() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.presentSequence;
    }

    uint64_t RhiCommandListExecutor::GetPresentRequestSequence() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.lastPresentRequestedSequence;
    }

    std::string RhiCommandListExecutor::GetSubmissionFailure(const uint64_t sequence) const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.failed && impl.firstFailedSequence != 0u && sequence >= impl.firstFailedSequence
            ? impl.failureDiagnostic : std::string{};
    }

    bool RhiCommandListExecutor::HasFailed() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.failed;
    }

    std::string RhiCommandListExecutor::GetFailureDiagnostic() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.failureDiagnostic;
    }

    size_t RhiCommandListExecutor::GetPendingTranslateCount() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.pendingTranslate.size();
    }

    size_t RhiCommandListExecutor::GetPendingSubmitCount() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.translated.size() + impl.nativeSubmissions.size();
    }

    bool RhiCommandListExecutor::IsSubmitInFlight() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.submitInFlight;
    }

    RhiThreadMode RhiCommandListExecutor::GetMode() const
    {
        const Impl& impl = State();
        std::lock_guard lock(impl.mutex);
        return impl.mode;
    }

    void RhiCommandListExecutor::ResetForTesting()
    {
        Impl& impl = State();
        std::deque<Impl::RetiredBatch> retired;
        {
            std::lock_guard lock(impl.mutex);
            impl.targets = {};
            impl.taskGraph = nullptr;
            impl.owner = nullptr;
            impl.mode = RhiThreadMode::None;
            impl.cpuWaitTimeout = kDefaultCpuWaitTimeout;
            impl.pendingTranslate.clear();
            impl.translated.clear();
            impl.nativeSubmissions.clear();
            retired.swap(impl.retireQueue);
            impl.quarantinedBatches.clear();
            impl.retiringResources.clear();
            impl.quarantinedResources.clear();
            impl.quarantinedTargets.clear();
            impl.resourceRetireInFlight = 0u;
            impl.dispatchSequence = 0u;
            impl.nextSubmitSequence = 1u;
            impl.lastTranslatedSequence = 0u;
            impl.lastPresentRequestedSequence = 0u;
            impl.submitInFlight = false;
            impl.submitSequence = 0u;
            impl.presentSequence = 0u;
            impl.lastGpuFenceSequence = 0u;
            impl.gpuCompletedSequence = 0u;
            impl.translateInFlight = 0u;
            impl.serialTranslationInFlight = false;
            impl.dispatches.clear();
            impl.failed = false;
            impl.firstFailedSequence = 0u;
            impl.failureDiagnostic.clear();
            impl.initialized = false;
            impl.acceptingNative = true;
        }
        for (const auto& batch : retired)
        {
            if (batch.retire)
                batch.retire(batch.translated);
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    // Dispatch / translate / submit pipes
    ///////////////////////////////////////////////////////////////////////////

    bool RhiCommandListExecutor::PumpTranslate()
    {
        Impl& impl = State();

        Impl::TranslateWork work;
        {
            std::lock_guard lock(impl.mutex);
            if (!impl.initialized || impl.failed || impl.pendingTranslate.empty() ||
                impl.translateInFlight >= impl.maxTranslateInFlight || impl.serialTranslationInFlight)
                return false;

            const auto runnable = std::find_if(
                impl.pendingTranslate.begin(), impl.pendingTranslate.end(),
                [&impl](const auto& candidate)
                { return candidate.batch->request.parallelTranslation || impl.translateInFlight == 0u; });
            if (runnable == impl.pendingTranslate.end())
                return false;
            work = std::move(*runnable);
            impl.pendingTranslate.erase(runnable);
            ++impl.translateInFlight;
            ++work.batch->activeTranslations;
            impl.serialTranslationInFlight = !work.batch->request.parallelTranslation;
        }
        const auto& list = work.batch->request.commandLists[work.listIndex];

        RhiTranslatedBatch batch;
        batch.sequence = list->GetSequence();
        batch.queueType = list->GetQueueType();
        batch.debugName = list->GetDebugName();
        batch.signalFence = list->GetSignalFence();
        batch.waitSemaphores = list->GetWaitSemaphores();
        batch.signalSemaphores = list->GetSignalSemaphores();
        batch.present = list->RequestsPresent();
        batch.offscreenOnly = list->IsOffscreenOnly();
        batch.commandCount = list->GetCommandCount();
        batch.submitEvent = list->GetSubmitEvent();
        batch.gpuEvent = list->GetGpuFence();
        batch.commandListKeepAlive = list;

        bool translated = false;
        std::string diagnostic = "RHI command list translation failed";
        try
        {
            if (!work.batch->request.acquireCommandBuffer)
            {
                translated = true;
            }
            else
            {
                batch.commandBuffer = work.batch->request.acquireCommandBuffer(
                    work.listIndex, batch.queueType, batch.sequence);
                if (batch.commandBuffer != nullptr)
                {
                    NLS_PROFILE_NAMED_SCOPE(
                        batch.debugName.empty() ? "RhiCommandListExecutor::Translate"
                                                : batch.debugName.c_str());
                    translated = list->TranslateNative(*batch.commandBuffer, &diagnostic);
                }
            }
        }
        catch (...)
        {
            translated = false;
            diagnostic = CurrentExceptionDiagnostic("RHI translate " + batch.debugName);
        }

        std::shared_ptr<Impl::DispatchedBatch> cancelled;
        {
            std::lock_guard lock(impl.mutex);
            if (impl.translateInFlight > 0u)
                --impl.translateInFlight;
            if (!work.batch->request.parallelTranslation)
                impl.serialTranslationInFlight = false;
            --work.batch->activeTranslations;
            --work.batch->remainingTranslations;
            if (impl.failed || !impl.initialized)
            {
                translated = false;
                diagnostic = impl.failed
                    ? impl.failureDiagnostic
                    : "RHI executor shut down during translation";
            }
            work.batch->translated[work.listIndex] = std::move(batch);
            if (translated)
                list->GetTranslateEvent().Signal(true);
            if (translated && work.batch->remainingTranslations == 0u)
            {
                impl.translated.emplace(work.batch->sequence, work.batch);
                impl.lastTranslatedSequence =
                    (std::max)(impl.lastTranslatedSequence, work.batch->sequence);
            }
            if (!translated)
            {
                impl.MarkFailedLocked(diagnostic);
                if (work.batch->activeTranslations == 0u &&
                    impl.dispatches.erase(work.batch->sequence) != 0u)
                    cancelled = work.batch;
            }
        }

        if (!translated)
        {
            list->GetTranslateEvent().Signal(false, diagnostic);
            list->GetSubmitEvent().Signal(false, diagnostic);
            list->GetGpuFence().Signal(false, diagnostic);
            FailPipeline(diagnostic);
            if (cancelled != nullptr)
                Impl::ReleaseCancelledBatch(*cancelled, diagnostic);
            return true;
        }

        impl.stateChanged.notify_all();
        ScheduleSubmitWork();
        return true;
    }

    bool RhiCommandListExecutor::PumpSubmit()
    {
        Impl& impl = State();
        if (SubmitLaneReenteredFlag())
            return false;
        std::lock_guard<std::mutex> laneLock(impl.submitLaneMutex);
        ScopedSubmitLaneFlag laneFlag;

        std::shared_ptr<Impl::DispatchedBatch> batch;
        std::shared_ptr<Impl::NativeSubmission> native;
        {
            std::lock_guard lock(impl.mutex);
            if (impl.failed || impl.submitInFlight)
                return false;
            if (impl.taskGraph != nullptr && impl.taskGraph->IsStarted())
            {
                if (impl.mode == RhiThreadMode::Tasks && !impl.taskGraph->IsWorkerThread())
                    return false;
                if (impl.mode == RhiThreadMode::DedicatedThread &&
                    impl.taskGraph->GetCurrentNamedThread() != NamedThread::Rhi)
                    return false;
            }

            // Only the next sequence may submit: parallel translation may finish
            // out of order, but submission stays FIFO.
            const auto foundNative = impl.nativeSubmissions.find(impl.nextSubmitSequence);
            if (foundNative != impl.nativeSubmissions.end())
            {
                native = std::move(foundNative->second);
                impl.nativeSubmissions.erase(foundNative);
            }
            else
            {
                const auto found = impl.translated.find(impl.nextSubmitSequence);
                if (!impl.initialized || found == impl.translated.end())
                    return false;
                batch = std::move(found->second);
                impl.translated.erase(found);
                impl.dispatches.erase(batch->sequence);
            }
            impl.submitInFlight = true;
        }

        // Marks the logical RHI lane: in Tasks mode this is what makes
        // IsInRhiThread() true even though a pool worker is executing.
        RenderingThreadSystem::ScopedRhiLaneScope rhiLaneScope;

        if (native != nullptr)
        {
            try
            {
                NLS_PROFILE_NAMED_SCOPE(
                    native->debugName.empty() ? "RhiCommandListExecutor::SubmitLane"
                                              : native->debugName.c_str());
                *native->result = native->action();
            }
            catch (...)
            {
                native->result->code = RHI::RHIQueueOperationStatusCode::BackendFailure;
                native->result->message = CurrentExceptionDiagnostic(
                    native->debugName.empty() ? "RHI native submit" : native->debugName);
                native->result->mayHaveQueuedGpuWork = true;
            }
            if (native->result->Succeeded() && HasFailed())
            {
                native->result->code = RHI::RHIQueueOperationStatusCode::BackendFailure;
                native->result->message = GetFailureDiagnostic();
                native->result->mayHaveQueuedGpuWork = true;
                native->result->frameFenceSignalQueued = false;
            }
            // An out-of-date swapchain is a frame-level condition, not a device-level
            // one: the swapchain has to be rebuilt and the lane must keep draining, so
            // it never marks the pipeline failed. The caller still receives the code
            // and decides whether that frame can proceed.
            const bool success =
                native->result->Succeeded() ||
                native->result->RequiresSwapchainRecreation();
            const std::string diagnostic = native->result->message.empty()
                ? std::string("RHI native queue operation failed") : native->result->message;
            {
                std::lock_guard lock(impl.mutex);
                if (!success)
                    impl.MarkFailedLocked(diagnostic);
                impl.submitInFlight = false;
                impl.submitSequence = (std::max)(impl.submitSequence, native->sequence);
                if (success && native->presents)
                    impl.presentSequence = (std::max)(impl.presentSequence, native->sequence);
                impl.nextSubmitSequence = (std::max)(impl.nextSubmitSequence, native->sequence + 1u);
            }
            if (!success)
                FailPipeline(diagnostic);
            native->action = {};
            native->completion.Signal(success, success ? std::string{} : diagnostic);
            impl.stateChanged.notify_all();
            if (success)
                ScheduleSubmitWork();
            return true;
        }

        bool success = true;
        std::string diagnostic;
        bool mayHaveQueuedGpuWork = false;
        bool retirementFenceQueued = false;
        try
        {
            NLS_PROFILE_NAMED_SCOPE(
                batch->request.debugName.empty() ? "RhiCommandListExecutor::Submit" : batch->request.debugName.c_str());

            if (batch->request.submit)
            {
                const RHI::RHIQueueOperationResult result = batch->request.submit(batch->translated);
                mayHaveQueuedGpuWork = result.mayHaveQueuedGpuWork ||
                    (result.Succeeded() && !batch->translated.empty());
                retirementFenceQueued = result.frameFenceSignalQueued;
                if (result.Succeeded() && batch->request.submitSignalsFences &&
                    batch->request.retirementFence == nullptr)
                    retirementFenceQueued = retirementFenceQueued || std::all_of(batch->translated.begin(), batch->translated.end(),
                        [](const auto& translated) { return translated.signalFence != nullptr; });
                if (!result.Succeeded() || HasFailed())
                {
                    success = false;
                    diagnostic = HasFailed() ? GetFailureDiagnostic() :
                        (result.message.empty() ? "RHI queue submit failed" : result.message);
                }
                else if (batch->request.commitResourceStates)
                {
                    for (const auto& translated : batch->translated)
                        if (translated.commandListKeepAlive != nullptr && translated.commandBuffer != nullptr)
                            translated.commandListKeepAlive->CommitResourceStates(*translated.commandBuffer);
                }
            }

            if (success && batch->request.requestsPresent && batch->request.present)
            {
                const RHI::RHIQueueOperationResult result = batch->request.present(batch->translated);
                retirementFenceQueued = retirementFenceQueued || result.frameFenceSignalQueued;
                mayHaveQueuedGpuWork = mayHaveQueuedGpuWork || result.mayHaveQueuedGpuWork;
                // A swapchain that went out of date is a recoverable present outcome:
                // the frame's GPU work is queued and retires through its fence, and
                // the caller rebuilds the swapchain instead of failing the pipeline.
                if (!result.Succeeded() && !result.RequiresSwapchainRecreation())
                {
                    success = false;
                    diagnostic = result.message.empty() ? "RHI present failed" : result.message;
                }
            }
        }
        catch (...)
        {
            success = false;
            mayHaveQueuedGpuWork = true;
            diagnostic = CurrentExceptionDiagnostic("RHI submit/present " + batch->request.debugName);
        }

        if (success && HasFailed())
        {
            success = false;
            diagnostic = GetFailureDiagnostic();
            mayHaveQueuedGpuWork = true;
        }
        if (success && batch->request.retirementFence != nullptr &&
            mayHaveQueuedGpuWork && !retirementFenceQueued)
        {
            success = false;
            diagnostic = "RHI batch did not queue its GPU retirement fence";
        }

        {
            std::lock_guard lock(impl.mutex);
            if (!success)
                impl.MarkFailedLocked(diagnostic);
            impl.submitInFlight = false;
            impl.submitSequence = (std::max)(impl.submitSequence, batch->sequence);
            if (success && batch->request.requestsPresent)
                impl.presentSequence = (std::max)(impl.presentSequence, batch->sequence);
            impl.nextSubmitSequence = (std::max)(impl.nextSubmitSequence, batch->sequence + 1u);
            for (auto& translated : batch->translated)
            {
                translated.gpuWorkQuarantined = !success && mayHaveQueuedGpuWork && !retirementFenceQueued;
                translated.submitEvent.Signal(success, diagnostic);
                if (!success)
                    translated.gpuEvent.Signal(false, diagnostic);
                auto retirementFence = batch->request.retirementFence != nullptr
                    ? batch->request.retirementFence : translated.signalFence;
                if (!success && !mayHaveQueuedGpuWork)
                    retirementFence.reset();
                impl.retireQueue.push_back({std::move(translated), std::move(retirementFence),
                    batch->request.retireCommandBuffer});
            }
        }

        if (!success)
        {
            FailPipeline(diagnostic);
            Impl::SignalCompletion(*batch, false, diagnostic);
            return true;
        }

        Impl::SignalCompletion(*batch, true);
        impl.stateChanged.notify_all();
        ScheduleSubmitWork();
        return true;
    }

    void RhiCommandListExecutor::ScheduleTranslateWork()
    {
        Impl& impl = State();
        TaskGraph* graph = nullptr;
        size_t concurrency = 1u;
        {
            std::lock_guard lock(impl.mutex);
            graph = impl.taskGraph;
            concurrency = (std::max)(static_cast<size_t>(1u), impl.maxTranslateInFlight);
        }

        // None still permits worker translation; its dispatching render thread
        // waits for the batch and performs the ordered submit itself.
        const bool graphUsable = graph != nullptr && graph->IsStarted();
        if (!graphUsable)
        {
            while (Pump())
            {
            }
            return;
        }

        for (size_t index = 0u; index < concurrency; ++index)
        {
            (void)graph->Submit(
                [this]
                {
                    while (PumpTranslate())
                    {
                    }
                },
                NamedThread::Any,
                TaskPriority::High,
                "RhiCommandListExecutor::Translate");
        }
    }

    void RhiCommandListExecutor::ScheduleSubmitWork()
    {
        Impl& impl = State();
        RhiThreadMode mode = RhiThreadMode::None;
        TaskGraph* graph = nullptr;
        {
            std::lock_guard lock(impl.mutex);
            mode = impl.mode;
            graph = impl.taskGraph;
            if (impl.failed || impl.submitInFlight)
                return;
            if (impl.nativeSubmissions.find(impl.nextSubmitSequence) == impl.nativeSubmissions.end() &&
                (!impl.initialized || impl.translated.find(impl.nextSubmitSequence) == impl.translated.end()))
                return;
        }

        if (mode == RhiThreadMode::None && graph != nullptr && graph->IsStarted())
            return;
        if (graph == nullptr || !graph->IsStarted())
        {
            while (PumpSubmit())
            {
            }
            return;
        }

        // Dedicated mode keeps the lane on its own thread; Tasks mode runs the
        // same logical lane on a high priority pool worker.
        const NamedThread target =
            mode == RhiThreadMode::DedicatedThread ? NamedThread::Rhi : NamedThread::Any;
        (void)graph->Submit(
            [this]
            {
                (void)PumpSubmit();
            },
            target,
            TaskPriority::High,
            "RhiCommandListExecutor::Submit");
    }

    void RhiCommandListExecutor::NotifyDeviceLost(std::string diagnostic)
    {
        FailPipeline(diagnostic.empty() ? "RHI device is lost" : std::move(diagnostic));
    }

    void RhiCommandListExecutor::FailPipeline(std::string diagnostic)
    {
        Impl& impl = State();

        std::vector<std::shared_ptr<Impl::DispatchedBatch>> abandoned;
        std::map<uint64_t, std::shared_ptr<Impl::NativeSubmission>> cancelledNative;
        {
            std::lock_guard lock(impl.mutex);
            impl.MarkFailedLocked(diagnostic);
            diagnostic = impl.failureDiagnostic;

            impl.pendingTranslate.clear();
            cancelledNative.swap(impl.nativeSubmissions);
            impl.translated.clear();
            for (auto it = impl.dispatches.begin(); it != impl.dispatches.end();)
            {
                Impl::SignalFailedLists(*it->second, diagnostic);
                if (it->second->activeTranslations == 0u)
                {
                    abandoned.push_back(it->second);
                    it = impl.dispatches.erase(it);
                }
                else
                    ++it;
            }
            for (auto& batch : impl.retireQueue)
                batch.translated.gpuEvent.Signal(false, diagnostic);
            for (auto& resources : impl.retiringResources)
                resources->completion.Signal(false, diagnostic);

            // Advance the observable sequences so fences and flushes blocked on
            // this work are released instead of hanging on a dead lane.
            impl.submitSequence = (std::max)(impl.submitSequence, impl.dispatchSequence);
            impl.presentSequence = (std::max)(impl.presentSequence, impl.dispatchSequence);
            impl.nextSubmitSequence = impl.dispatchSequence + 1u;
        }

        for (auto& [sequence, operation] : cancelledNative)
        {
            (void)sequence;
            operation->result->code = RHI::RHIQueueOperationStatusCode::BackendFailure;
            operation->result->message = diagnostic;
            operation->result->mayHaveQueuedGpuWork = false;
            operation->action = {};
            operation->completion.Signal(false, diagnostic);
        }
        for (const auto& batch : abandoned)
            Impl::ReleaseCancelledBatch(*batch, diagnostic);

        impl.stateChanged.notify_all();
        NLS_LOG_ERROR("RhiCommandListExecutor failed: " + diagnostic);
    }

    bool RhiCommandListExecutor::Pump()
    {
        bool didWork = PumpTranslate();
        if (PumpSubmit())
            didWork = true;
        return didWork;
    }

    bool RhiCommandListExecutor::Flush(const RhiFlushFlags flags)
    {
        Impl& impl = State();
        const auto started = std::chrono::steady_clock::now();

        // Deeper stages imply the shallower ones: asking for Submit necessarily
        // requires dispatch and translate to have happened.
        const bool waitSubmit = HasFlag(flags, RhiFlushFlags::Submit) ||
            HasFlag(flags, RhiFlushFlags::Gpu) ||
            HasFlag(flags, RhiFlushFlags::Swapchain);
        const bool waitTranslate = HasFlag(flags, RhiFlushFlags::Translate) || waitSubmit;
        const bool waitGpu = HasFlag(flags, RhiFlushFlags::Gpu);
        const bool waitPresent = HasFlag(flags, RhiFlushFlags::Swapchain);
        // Resource release is orthogonal: it retires every batch whose GPU fence
        // already signalled, and must not silently require a GPU wait.
        const bool releaseResources = HasFlag(flags, RhiFlushFlags::ReleaseResources);

        if (waitGpu)
        {
            bool needsFence = false;
            {
                std::lock_guard fenceLock(impl.mutex);
                needsFence = !impl.failed && impl.acceptingNative && impl.targets.signalGpuFences &&
                    impl.gpuCompletedSequence < impl.dispatchSequence &&
                    impl.lastGpuFenceSequence < impl.dispatchSequence;
            }
            if (needsFence)
                (void)InsertGpuFence();
        }

        std::unique_lock lock(impl.mutex);
        const auto deadline = started + impl.cpuWaitTimeout;
        while (true)
        {
            if (impl.failed)
                return false;

            // Dispatch assigns sequence and dependencies before returning.
            const bool dispatchDone = true;
            const bool translateDone = !waitTranslate ||
                (impl.pendingTranslate.empty() &&
                 impl.translateInFlight == 0u &&
                 impl.lastTranslatedSequence >= impl.dispatchSequence);
            const bool submitDone = !waitSubmit ||
                (impl.nextSubmitSequence > impl.dispatchSequence &&
                 !impl.submitInFlight);
            const bool presentDone = !waitPresent ||
                impl.presentSequence >= impl.lastPresentRequestedSequence;
            const bool gpuDone = !waitGpu ||
                (impl.retireQueue.empty() && impl.retiringResources.empty() && impl.resourceRetireInFlight == 0u);

            if (dispatchDone && translateDone && submitDone && presentDone && gpuDone)
                break;

            if (std::chrono::steady_clock::now() >= deadline)
            {
                NLS_LOG_ERROR("RhiCommandListExecutor::Flush timed out");
                return false;
            }

            if (impl.mode == RhiThreadMode::None || impl.taskGraph == nullptr || !impl.taskGraph->IsStarted())
            {
                lock.unlock();
                (void)Pump();
                lock.lock();
            }

            if (releaseResources)
            {
                // Retire GPU-complete batches without waiting for the GPU.
                lock.unlock();
                (void)ReleaseRetiredBatches();
                lock.lock();
            }

            if (waitGpu)
            {
                // GPU completion is polled on the backend fence, so the wait has
                // to leave the lock instead of blocking on a condition variable.
                lock.unlock();
                (void)ReleaseRetiredBatches();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                lock.lock();
                continue;
            }

            impl.stateChanged.wait_for(lock, std::chrono::milliseconds(2));
        }

        lock.unlock();
        if (releaseResources)
        {
            // Final pass so a caller that asked for release sees every already
            // GPU-complete batch retired before this call returns.
            (void)ReleaseRetiredBatches();
        }
        return true;
    }

    size_t RhiCommandListExecutor::ReleaseRetiredBatches()
    {
        Impl& impl = State();

        std::vector<Impl::RetiredBatch> completed;
        std::vector<std::shared_ptr<Impl::RetiringResources>> releasedResources;
        {
            std::lock_guard lock(impl.mutex);
            for (auto it = impl.retireQueue.begin(); it != impl.retireQueue.end();)
            {
                auto& retired = *it;
                RhiTranslatedBatch& batch = retired.translated;
                // Custom queue plans retire on the whole frame's fence without
                // changing the per-list fence passed to native queue submission.
                const bool gpuComplete =
                    !batch.gpuWorkQuarantined &&
                    (batch.sequence <= impl.gpuCompletedSequence ||
                        retired.retirementFence == nullptr || retired.retirementFence->IsSignaled());
                if (!gpuComplete)
                    ++it;
                else
                {
                    completed.push_back(std::move(retired));
                    it = impl.retireQueue.erase(it);
                }
            }
            for (auto it = impl.retiringResources.begin(); it != impl.retiringResources.end();)
            {
                const auto& resources = *it;
                if (resources->completion.IsComplete() && !resources->completion.Succeeded())
                {
                    impl.quarantinedResources.push_back(std::move(*it));
                    it = impl.retiringResources.erase(it);
                }
                else if (resources->submitted && std::all_of(resources->fences.begin(), resources->fences.end(),
                    [](const auto& fence) { return fence->IsSignaled(); }))
                {
                    ++impl.resourceRetireInFlight;
                    releasedResources.push_back(std::move(*it));
                    it = impl.retiringResources.erase(it);
                }
                else
                    ++it;
            }
        }

        for (auto& batch : completed)
        {
            batch.translated.gpuEvent.Signal(true);
            if (batch.retire)
                batch.retire(batch.translated);
        }
        for (auto& resources : releasedResources)
        {
            resources->owners.clear();
            resources->completion.Signal(true);
            {
                std::lock_guard lock(impl.mutex);
                impl.gpuCompletedSequence = (std::max)(impl.gpuCompletedSequence,
                    resources->completion.GetSequence());
                --impl.resourceRetireInFlight;
            }
        }

        if (!completed.empty() || !releasedResources.empty())
            impl.stateChanged.notify_all();
        return completed.size() + releasedResources.size();
    }

    ///////////////////////////////////////////////////////////////////////////
    // RhiCommandList
    ///////////////////////////////////////////////////////////////////////////

    void RhiCommandList::SetDebugName(std::string debugName)
    {
        if (m_state != State::Recording)
            return;
        m_debugName = std::move(debugName);
    }

    void RhiCommandList::SetQueueType(const RHI::QueueType queueType)
    {
        if (m_state != State::Recording)
            return;
        m_queueType = queueType;
    }

    void RhiCommandList::SetSignalFence(std::shared_ptr<RHI::RHIFence> fence)
    {
        if (m_state != State::Recording)
            return;
        m_signalFence = std::move(fence);
    }

    void RhiCommandList::AddWaitSemaphore(std::shared_ptr<RHI::RHISemaphore> semaphore)
    {
        if (m_state != State::Recording || semaphore == nullptr)
            return;
        m_waitSemaphores.push_back(std::move(semaphore));
    }

    void RhiCommandList::AddSignalSemaphore(std::shared_ptr<RHI::RHISemaphore> semaphore)
    {
        if (m_state != State::Recording || semaphore == nullptr)
            return;
        m_signalSemaphores.push_back(std::move(semaphore));
    }

    void RhiCommandList::RequestPresent(const bool present)
    {
        if (m_state != State::Recording)
            return;
        m_present = present;
    }

    void RhiCommandList::MarkOffscreenOnly(const bool offscreenOnly)
    {
        if (m_state != State::Recording)
            return;
        m_offscreenOnly = offscreenOnly;
    }

#if defined(NLS_ENABLE_TEST_HOOKS)
    void RhiCommandList::SetRecordObserverForTesting(RecordObserverForTesting observer)
    {
        s_recordObserverForTesting = std::move(observer);
    }

    void RhiCommandList::ClearRecordObserverForTesting()
    {
        s_recordObserverForTesting = nullptr;
    }
#endif
    bool RhiCommandList::Record(std::string debugName, RecordFunction body)
    {
        if (m_state != State::Recording || !body)
            return false;

        m_commands.push_back(RecordedCommand { std::move(debugName), std::move(body) });
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (s_recordObserverForTesting != nullptr)
        s_recordObserverForTesting(*this, m_commands.back().debugName);
#endif
        return true;
    }

    bool RhiCommandList::RecordBarrier(RHI::RHIBarrierDesc barrier, std::string debugName)
    {
        if (!IsRecording())
            return false;
        if (barrier.statesAreExplicit)
            m_resourceStateTransitions.push_back(barrier);
        return Record(
            debugName.empty() ? std::string("Barrier") : std::move(debugName),
            [barrier = std::move(barrier)](RHI::RHICommandBuffer& commandBuffer)
            {
                const auto result = commandBuffer.BarrierChecked(barrier);
                if (!result.Succeeded())
                    throw std::runtime_error(result.message.empty() ? "RHI barrier translation failed" : result.message);
            });
    }

    void RhiCommandList::Close()
    {
        if (m_state != State::Recording)
            return;
        m_state = State::Closed;
    }

    bool RhiCommandList::Submit()
    {
        if (m_state != State::Closed)
        {
            GetDispatchEvent().Signal(false, "RHI command list must be closed before Submit");
            return false;
        }

        // A list must be owned by a shared_ptr for the executor to outlive the
        // caller; a stack instance is rejected instead of throwing bad_weak_ptr.
        const std::shared_ptr<RhiCommandList> self = weak_from_this().lock();
        if (self == nullptr)
        {
            GetDispatchEvent().Signal(
                false,
                "RHI command list must be owned by a std::shared_ptr to be submitted");
            return false;
        }

        return RhiCommandListExecutor::Get().Dispatch(self);
    }

    bool RhiCommandList::Translate(
        RHI::RHICommandBuffer& commandBuffer,
        std::string* diagnostic) const
    {
        // A zone per replayed command would dominate the frame it measures: a
        // single deferred pass replays thousands of binds and draws, and every
        // profiler scope copies the destination list, allocates its strings and
        // takes the global profiler lock. The batch-level scope in
        // RhiCommandListExecutor::Translate reports the same stage without the
        // per-command cost; deep per-command attribution is opt-in.
        const bool profileEveryCommand =
            Settings::GetThreadDiagnosticsSettings().profileEveryRhiCommand;
        for (const RecordedCommand& command : m_commands)
        {
            if (!command.body)
                continue;

            try
            {
                if (profileEveryCommand)
                {
                    NLS_PROFILE_NAMED_SCOPE(
                        command.debugName.empty() ? "RhiCommandList::Command" : command.debugName.c_str());
                    command.body(commandBuffer);
                }
                else
                {
                    command.body(commandBuffer);
                }
            }
            catch (...)
            {
                const auto reason = CurrentExceptionDiagnostic(
                    "RhiCommandList::Translate " +
                    (command.debugName.empty() ? std::string("<unnamed>") : command.debugName));
                if (diagnostic != nullptr)
                    *diagnostic = reason;
                NLS_LOG_ERROR(reason);
                return false;
            }
        }
        return true;
    }

    bool RhiCommandList::TranslateNative(
        RHI::RHICommandBuffer& commandBuffer,
        std::string* diagnostic) const
    {
        std::string reason;
        bool succeeded = false;
        try
        {
            if (IsRecording())
            {
                reason = "Native translation requires a closed RHI command list";
            }
            else
            {
                commandBuffer.Begin();
                if (!commandBuffer.IsRecording())
                    reason = "Native command list Begin failed";
                else
                    succeeded = Translate(commandBuffer, &reason);
                if (commandBuffer.IsRecording())
                    commandBuffer.End();
                if (succeeded && !commandBuffer.IsClosedForSubmission())
                {
                    succeeded = false;
                    reason = "Native command list close failed";
                }
            }
        }
        catch (...)
        {
            succeeded = false;
            reason = CurrentExceptionDiagnostic("Native command list translation");
        }
        if (!succeeded)
        {
            try
            {
                if (commandBuffer.IsRecording())
                    commandBuffer.End();
            }
            catch (...)
            {
                reason += "; " + CurrentExceptionDiagnostic("Native command list cleanup");
            }
        }
        if (diagnostic != nullptr)
            *diagnostic = std::move(reason);
        return succeeded;
    }

    void RhiCommandList::CommitResourceStates(RHI::RHICommandBuffer& commandBuffer) const
    {
        for (const auto& barrier : m_resourceStateTransitions)
            commandBuffer.CommitResourceStates(barrier);
    }
}
