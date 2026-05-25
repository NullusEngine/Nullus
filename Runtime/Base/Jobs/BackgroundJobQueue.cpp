#include "Jobs/BackgroundJobQueue.h"

#include "Jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Jobs/JobDiagnostics.h"
#include "Profiling/Profiler.h"

namespace NLS::Base::Jobs
{
namespace
{
    constexpr uint64_t kBackgroundHandleBit = 1ull << 63u;
    constexpr size_t kRetiredBackgroundHandleHistoryLimit = 4096u;
    constexpr uint32_t kMaxBackgroundWorkerCount = 64u;
    std::atomic<uint32_t> g_nextBackgroundQueueGeneration{1u};

    uint32_t AllocateBackgroundQueueGeneration()
    {
        uint32_t generation = 0u;
        do
        {
            generation = g_nextBackgroundQueueGeneration.fetch_add(1u, std::memory_order_relaxed);
        } while (generation == 0u);
        return generation;
    }

    void RunUserCallback(
        const JobFunction function,
        void* userData,
        const uint64_t jobId,
        const char* message)
    {
        if (function == nullptr)
            return;
        try
        {
            function(userData);
        }
        catch (...)
        {
            Internal::RecordJobViolation(
                JobViolationKind::CallbackException,
                jobId,
                message);
        }
    }

    class JobWorkerExecutionScope
    {
    public:
        explicit JobWorkerExecutionScope(const JobHandle handle)
        {
            Internal::EnterJobWorkerExecution(handle);
        }

        ~JobWorkerExecutionScope()
        {
            Internal::ExitJobWorkerExecution();
        }

        JobWorkerExecutionScope(const JobWorkerExecutionScope&) = delete;
        JobWorkerExecutionScope& operator=(const JobWorkerExecutionScope&) = delete;
    };

    void RunUserCallbackForJobWorker(
        const JobFunction function,
        void* userData,
        const JobHandle handle,
        const char* message)
    {
        if (function == nullptr)
            return;

        JobWorkerExecutionScope workerExecutionScope(handle);
        RunUserCallback(function, userData, handle.id, message);
    }

    enum class BackgroundJobState : uint8_t
    {
        WaitingForDependency = 0,
        Queued,
        Running,
        Completed,
        Cancelled,
        Failed
    };

    struct BackgroundJob
    {
        uint64_t id = 0u;
        uint32_t generation = 1u;
        BackgroundJobDesc desc;
        std::string debugName;
        BackgroundJobState state = BackgroundJobState::WaitingForDependency;
        bool terminalCallbackRunning = false;
    };

    struct BackgroundJobTerminalCallback
    {
        std::shared_ptr<BackgroundJob> job;
        JobFunction function = nullptr;
        void* userData = nullptr;
    };

    struct Continuation
    {
        MainThreadContinuationDesc desc;
        uint64_t sequence = 0u;
        bool retainedExternalDependency = false;
    };

    class BackgroundQueue
    {
    public:
        ~BackgroundQueue()
        {
            Shutdown(JobSystemShutdownMode::Immediate);
        }

        bool Start(uint32_t workerCount)
        {
            Shutdown(JobSystemShutdownMode::Immediate);

            {
                std::lock_guard lock(m_mutex);
                m_acceptingWork = true;
                m_shutdownRequested = false;
                m_queueGeneration = AllocateBackgroundQueueGeneration();
            }

            workerCount = std::clamp(workerCount, 1u, kMaxBackgroundWorkerCount);
            try
            {
                m_workers.reserve(workerCount);
                for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex)
                    m_workers.emplace_back([this, workerIndex] { WorkerLoop(workerIndex); });
            }
            catch (...)
            {
                Shutdown(JobSystemShutdownMode::Immediate);
                return false;
            }

            return true;
        }

        void Shutdown(const JobSystemShutdownMode mode)
        {
            std::vector<BackgroundJobTerminalCallback> cancelCallbacks;
            if (mode == JobSystemShutdownMode::DrainAcceptedWork)
            {
                {
                    std::lock_guard lock(m_mutex);
                    m_acceptingWork = false;
                }
                m_workAvailable.notify_all();

                while (true)
                {
                    {
                        std::lock_guard lock(m_mutex);
                        const bool unfinished = std::any_of(
                            m_jobs.begin(),
                            m_jobs.end(),
                            [](const auto& entry)
                            {
                                const auto& job = entry.second;
                                return job != nullptr &&
                                    job->state != BackgroundJobState::Completed &&
                                    job->state != BackgroundJobState::Cancelled &&
                                    job->state != BackgroundJobState::Failed;
                            });

                        if (!unfinished)
                            break;
                    }

                    WakeDependencyReadyJobs();
                    HelpDependencies();
                    std::this_thread::yield();
                }
            }

            std::vector<std::thread> workers;
            bool notifyForegroundDependency = false;
            {
                std::lock_guard lock(m_mutex);
                m_acceptingWork = false;
                m_shutdownRequested = true;
                if (mode == JobSystemShutdownMode::Immediate)
                {
                    std::vector<std::shared_ptr<BackgroundJob>> jobsToCancel;
                    jobsToCancel.reserve(m_jobs.size());
                    for (auto& [id, job] : m_jobs)
                    {
                        (void)id;
                        if (job != nullptr &&
                            (job->state == BackgroundJobState::WaitingForDependency ||
                                job->state == BackgroundJobState::Queued))
                        {
                            jobsToCancel.push_back(job);
                        }
                    }

                    for (const auto& job : jobsToCancel)
                    {
                        if (job == nullptr ||
                            (job->state != BackgroundJobState::WaitingForDependency &&
                                job->state != BackgroundJobState::Queued))
                        {
                            continue;
                        }

                        job->state = BackgroundJobState::Cancelled;
                        QueueTerminalCallbackLocked(job, cancelCallbacks);
                    }
                }
                workers = std::move(m_workers);
            }
            m_workAvailable.notify_all();

            for (const auto& callback : cancelCallbacks)
            {
                const auto& job = callback.job;
                if (job == nullptr)
                    continue;
                RunUserCallbackForJobWorker(
                    callback.function,
                    callback.userData,
                    {kBackgroundHandleBit | job->id, job->generation},
                    "Background job cancel callback threw an exception.");
            }
            notifyForegroundDependency |= RetireJobsAfterTerminalCallbacks(cancelCallbacks);
            NotifyForegroundDependencyChangedIfNeeded(notifyForegroundDependency);

            for (auto& worker : workers)
            {
                if (worker.joinable())
                    worker.join();
            }

            {
                std::lock_guard lock(m_mutex);
                m_readyJobs.clear();
                ClearContinuationsLocked();
                m_jobs.clear();
                m_shutdownRequested = false;
            }
        }

        void ClearRetiredHistory()
        {
            std::lock_guard lock(m_mutex);
            m_retiredGenerations.clear();
            m_retiredStatuses.clear();
            m_retiredOrder.clear();
        }

        void StopAcceptingWork()
        {
            std::lock_guard lock(m_mutex);
            m_acceptingWork = false;
        }

        void NotifyDependencyChanged()
        {
            {
                std::lock_guard lock(m_mutex);
                m_dependencyChanged = true;
            }
            m_workAvailable.notify_all();
        }

        static void NotifyForegroundDependencyChangedIfNeeded(const bool needed)
        {
            if (needed)
                Internal::NotifyForegroundDependencyChanged();
        }

        JobHandle Schedule(const BackgroundJobDesc& desc)
        {
            if (desc.function == nullptr)
            {
                Internal::RecordJobViolation(
                    JobViolationKind::NullCallback,
                    0u,
                    "Background job scheduling received a null callback.");
                return {};
            }

            if (!IsSchedulableDependency(desc.dependency))
            {
                Internal::RecordJobViolation(
                    JobViolationKind::InvalidHandle,
                    desc.dependency.id,
                    "Background job scheduling received an unknown dependency handle.");
                return {};
            }

            const bool externalDependency = desc.dependency.id != 0u && !OwnsHandle(desc.dependency);
            bool retainedExternalDependency = false;
            const auto externalDependencyStatus = externalDependency
                ? Internal::RetainJobCompletionStatus(desc.dependency, retainedExternalDependency)
                : JobCompletionStatus::Unknown;
            if (externalDependency && externalDependencyStatus == JobCompletionStatus::Unknown)
            {
                Internal::RecordJobViolation(
                    JobViolationKind::InvalidHandle,
                    desc.dependency.id,
                    "Background job scheduling received an external dependency that could not be retained.");
                return {};
            }

            JobHandle handle;
            std::vector<BackgroundJobTerminalCallback> cancelCallbacks;
            bool notifyWorker = false;
            bool notifyForegroundDependency = false;
            {
                std::lock_guard lock(m_mutex);
                if (!m_acceptingWork || m_shutdownRequested)
                {
                    if (retainedExternalDependency)
                        Internal::ReleaseJobCompletionStatus(desc.dependency);
                    Internal::RecordJobViolation(
                        JobViolationKind::ShutdownSchedulingRejected,
                        0u,
                        "Background job scheduling was rejected while the JobSystem is shutting down or not accepting work.");
                    return {};
                }

                auto job = std::make_shared<BackgroundJob>();
                job->id = m_nextJobId++;
                job->generation = m_queueGeneration;
                job->desc = desc;
                job->debugName = desc.debugName != nullptr ? desc.debugName : "";
                job->desc.debugName = job->debugName.empty() ? nullptr : job->debugName.c_str();
                handle = {kBackgroundHandleBit | job->id, job->generation};
                m_jobs.emplace(job->id, job);
                Internal::RecordJobDiagnostic(
                    handle.id,
                    job->generation,
                    JobLifecycleState::Created,
                    job->desc.debugName,
                    nullptr,
                    desc.dependency.id == 0u ? 0u : 1u);

                const auto dependencyStatus = externalDependency
                    ? externalDependencyStatus
                    : GetDependencyStatusLocked(desc.dependency);
                if (dependencyStatus == JobCompletionStatus::Succeeded)
                {
                    job->state = BackgroundJobState::Queued;
                    Internal::RecordJobDiagnostic(
                        handle.id,
                        job->generation,
                        JobLifecycleState::Queued,
                        job->desc.debugName,
                        nullptr,
                        desc.dependency.id == 0u ? 0u : 1u);
                    m_readyJobs.push_back(job->id);
                    notifyWorker = true;
                }
                else if (dependencyStatus == JobCompletionStatus::Cancelled ||
                    dependencyStatus == JobCompletionStatus::Failed)
                {
                    job->state = BackgroundJobState::Cancelled;
                    Internal::RecordJobDiagnostic(
                        handle.id,
                        job->generation,
                        JobLifecycleState::Cancelled,
                        job->desc.debugName,
                        nullptr,
                        desc.dependency.id == 0u ? 0u : 1u);
                    QueueTerminalCallbackLocked(job, cancelCallbacks);
                }
                else
                {
                    job->state = BackgroundJobState::WaitingForDependency;
                    Internal::RecordJobDiagnostic(
                        handle.id,
                        job->generation,
                        JobLifecycleState::WaitingForDependencies,
                        job->desc.debugName,
                        nullptr,
                        desc.dependency.id == 0u ? 0u : 1u);
                    notifyWorker = true;
                }
            }

            if (notifyWorker)
                m_workAvailable.notify_one();
            for (const auto& callback : cancelCallbacks)
            {
                if (callback.job == nullptr)
                    continue;
                RunUserCallbackForJobWorker(
                    callback.function,
                    callback.userData,
                    {kBackgroundHandleBit | callback.job->id, callback.job->generation},
                    "Background job cancel callback threw an exception.");
            }
            notifyForegroundDependency |= RetireJobsAfterTerminalCallbacks(cancelCallbacks);
            NotifyForegroundDependencyChangedIfNeeded(notifyForegroundDependency);

            return handle;
        }

        bool ScheduleContinuation(const MainThreadContinuationDesc& desc)
        {
            if (desc.function == nullptr)
            {
                Internal::RecordJobViolation(
                    JobViolationKind::NullCallback,
                    0u,
                    "Main-thread continuation scheduling received a null callback.");
                return false;
            }

            if (!IsSchedulableDependency(desc.dependency))
            {
                Internal::RecordJobViolation(
                    JobViolationKind::InvalidHandle,
                    desc.dependency.id,
                    "Main-thread continuation scheduling received an unknown dependency handle.");
                return false;
            }

            const bool externalDependency = desc.dependency.id != 0u && !OwnsHandle(desc.dependency);
            bool retainedExternalDependency = false;
            const auto externalDependencyStatus = externalDependency
                ? Internal::RetainJobCompletionStatus(desc.dependency, retainedExternalDependency)
                : JobCompletionStatus::Unknown;
            if (externalDependency && externalDependencyStatus == JobCompletionStatus::Unknown)
            {
                Internal::RecordJobViolation(
                    JobViolationKind::InvalidHandle,
                    desc.dependency.id,
                    "Main-thread continuation scheduling received an external dependency that could not be retained.");
                return false;
            }

            std::lock_guard lock(m_mutex);
            if (!m_acceptingWork || m_shutdownRequested)
            {
                if (retainedExternalDependency)
                    Internal::ReleaseJobCompletionStatus(desc.dependency);
                Internal::RecordJobViolation(
                    JobViolationKind::ShutdownSchedulingRejected,
                    0u,
                    "Main-thread continuation scheduling was rejected while the JobSystem is shutting down or not accepting work.");
                return false;
            }

            auto continuation = Continuation{desc, m_nextContinuationSequence++, retainedExternalDependency};
            auto dependencyStatus = externalDependency
                ? externalDependencyStatus
                : GetDependencyStatusLocked(desc.dependency);
            if (dependencyStatus == JobCompletionStatus::Succeeded)
            {
                if (continuation.retainedExternalDependency)
                    Internal::ReleaseJobCompletionStatus(continuation.desc.dependency);
                continuation.retainedExternalDependency = false;
                m_readyContinuations.push_back(continuation);
            }
            else if (dependencyStatus == JobCompletionStatus::Cancelled ||
                dependencyStatus == JobCompletionStatus::Failed)
            {
                m_continuations.push_back(continuation);
            }
            else
            {
                m_continuations.push_back(continuation);
            }
            return true;
        }

        uint32_t DrainContinuations(const uint32_t maxContinuations)
        {
            std::vector<std::pair<JobHandle, JobCompletionStatus>> resolvedExternalDependencies;
            std::vector<JobHandle> externalDependenciesToCheck;
            {
                std::lock_guard lock(m_mutex);
                for (const auto& continuation : m_continuations)
                {
                    const JobHandle dependency = continuation.desc.dependency;
                    if (dependency.id == 0u || OwnsHandle(dependency))
                        continue;

                    const bool alreadyQueued = std::any_of(
                        externalDependenciesToCheck.begin(),
                        externalDependenciesToCheck.end(),
                        [dependency](const JobHandle existing)
                        {
                            return existing.id == dependency.id &&
                                existing.generation == dependency.generation;
                        });
                    if (!alreadyQueued)
                        externalDependenciesToCheck.push_back(dependency);
                }
            }

            resolvedExternalDependencies.reserve(externalDependenciesToCheck.size());
            for (const JobHandle dependency : externalDependenciesToCheck)
            {
                Internal::KickPendingJobForBatch(dependency);
                const auto status = Internal::GetJobCompletionStatus(dependency);
                if (status == JobCompletionStatus::Succeeded ||
                    status == JobCompletionStatus::Cancelled ||
                    status == JobCompletionStatus::Failed)
                    resolvedExternalDependencies.emplace_back(dependency, status);
            }

            {
                std::lock_guard lock(m_mutex);
                PromoteReadyContinuationsLocked(resolvedExternalDependencies);
            }

            uint32_t drained = 0u;
            while (maxContinuations == 0u || drained < maxContinuations)
            {
                Continuation continuation;
                {
                    std::lock_guard lock(m_mutex);
                    if (m_readyContinuations.empty())
                        break;

                    continuation = m_readyContinuations.front();
                    m_readyContinuations.pop_front();
                }

                RunUserCallback(
                    continuation.desc.function,
                    continuation.desc.userData,
                    0u,
                    "Main-thread continuation callback threw an exception.");
                ++drained;
            }

            return drained;
        }

        void PromoteReadyContinuationsLocked(
            const std::vector<std::pair<JobHandle, JobCompletionStatus>>& resolvedExternalDependencies)
        {
            auto iter = m_continuations.begin();
            while (iter != m_continuations.end())
            {
                auto dependencyStatus = GetDependencyStatusLocked(iter->desc.dependency);
                if (dependencyStatus == JobCompletionStatus::Unknown &&
                    iter->desc.dependency.id != 0u &&
                    !OwnsHandle(iter->desc.dependency))
                {
                    const auto foundDependencyStatus = std::find_if(
                        resolvedExternalDependencies.begin(),
                        resolvedExternalDependencies.end(),
                        [dependency = iter->desc.dependency](const std::pair<JobHandle, JobCompletionStatus>& resolved)
                        {
                            return resolved.first.id == dependency.id &&
                                resolved.first.generation == dependency.generation;
                        });
                    if (foundDependencyStatus != resolvedExternalDependencies.end())
                        dependencyStatus = foundDependencyStatus->second;
                }

                if (dependencyStatus == JobCompletionStatus::Cancelled ||
                    dependencyStatus == JobCompletionStatus::Failed)
                {
                    if (iter->retainedExternalDependency)
                        Internal::ReleaseJobCompletionStatus(iter->desc.dependency);
                    iter = m_continuations.erase(iter);
                    continue;
                }

                if (dependencyStatus != JobCompletionStatus::Succeeded)
                {
                    ++iter;
                    continue;
                }

                Continuation continuation = *iter;
                if (continuation.retainedExternalDependency)
                    Internal::ReleaseJobCompletionStatus(continuation.desc.dependency);
                continuation.retainedExternalDependency = false;
                iter = m_continuations.erase(iter);
                m_readyContinuations.push_back(continuation);
            }
        }

        bool OwnsHandle(const JobHandle handle) const
        {
            return (handle.id & kBackgroundHandleBit) != 0u;
        }

        bool IsSchedulableDependency(const JobHandle dependency) const
        {
            if (dependency.id == 0u && dependency.generation == 0u)
                return true;

            if (!OwnsHandle(dependency))
                return NLS::Base::Jobs::Internal::IsKnownJobHandle(dependency);

            std::lock_guard lock(m_mutex);
            return FindJobLocked(dependency) != nullptr || IsRetiredHandleLocked(dependency);
        }

        bool Completed(const JobHandle handle) const
        {
            if (!OwnsHandle(handle))
                return false;

            std::lock_guard lock(m_mutex);
            if (IsRetiredHandleLocked(handle))
                return true;

            auto job = FindJobLocked(handle);
            if (job == nullptr)
            {
                Internal::RecordJobViolation(
                    JobViolationKind::StaleHandle,
                    handle.id,
                    "Completion query received an unknown or stale background job handle.");
                return true;
            }

            return !job->terminalCallbackRunning &&
                (job->state == BackgroundJobState::Completed ||
                    job->state == BackgroundJobState::Cancelled ||
                    job->state == BackgroundJobState::Failed);
        }

        JobCompletionStatus CompletionStatus(const JobHandle handle) const
        {
            if (!OwnsHandle(handle))
                return JobCompletionStatus::Unknown;

            std::lock_guard lock(m_mutex);
            if (IsRetiredHandleLocked(handle))
                return GetRetiredStatusLocked(handle);

            auto job = FindJobLocked(handle);
            if (job == nullptr)
                return JobCompletionStatus::Unknown;

            return GetJobStatusLocked(job);
        }

        bool Known(const JobHandle handle) const
        {
            if (!OwnsHandle(handle))
                return false;

            std::lock_guard lock(m_mutex);
            return FindJobLocked(handle) != nullptr || IsRetiredHandleLocked(handle);
        }

        static void HelpForegroundDependency(const JobHandle dependency)
        {
            if (dependency.id == 0u)
                return;

            Internal::KickPendingJobForBatch(dependency);
            (void)Internal::ExecuteForegroundJobForWait(dependency, false);
        }

        void CollectForegroundDependenciesForBackgroundChainLocked(
            const JobHandle handle,
            std::vector<JobHandle>& foregroundDependencies,
            std::vector<JobHandle>& visitedBackgroundHandles) const
        {
            if (handle.id == 0u)
                return;

            if (!OwnsHandle(handle))
            {
                const bool alreadyQueued = std::any_of(
                    foregroundDependencies.begin(),
                    foregroundDependencies.end(),
                    [handle](const JobHandle existing)
                    {
                        return existing.id == handle.id &&
                            existing.generation == handle.generation;
                    });
                if (!alreadyQueued)
                    foregroundDependencies.push_back(handle);
                return;
            }

            const bool alreadyVisited = std::any_of(
                visitedBackgroundHandles.begin(),
                visitedBackgroundHandles.end(),
                [handle](const JobHandle existing)
                {
                    return existing.id == handle.id &&
                        existing.generation == handle.generation;
                });
            if (alreadyVisited)
                return;
            visitedBackgroundHandles.push_back(handle);

            auto job = FindJobLocked(handle);
            if (job == nullptr ||
                job->state == BackgroundJobState::Completed ||
                job->state == BackgroundJobState::Cancelled ||
                job->state == BackgroundJobState::Failed)
            {
                return;
            }

            CollectForegroundDependenciesForBackgroundChainLocked(
                job->desc.dependency,
                foregroundDependencies,
                visitedBackgroundHandles);
        }

        void CollectBackgroundDependencyChainLocked(
            const JobHandle handle,
            std::vector<JobHandle>& backgroundHandles) const
        {
            if (!OwnsHandle(handle))
                return;

            const bool alreadyVisited = std::any_of(
                backgroundHandles.begin(),
                backgroundHandles.end(),
                [handle](const JobHandle existing)
                {
                    return existing.id == handle.id &&
                        existing.generation == handle.generation;
                });
            if (alreadyVisited)
                return;

            auto job = FindJobLocked(handle);
            if (job == nullptr ||
                job->state == BackgroundJobState::Completed ||
                job->state == BackgroundJobState::Cancelled ||
                job->state == BackgroundJobState::Failed)
            {
                return;
            }

            backgroundHandles.push_back(handle);
            CollectBackgroundDependencyChainLocked(job->desc.dependency, backgroundHandles);
        }

        void HelpDependencies()
        {
            std::vector<JobHandle> foregroundDependencies;
            {
                std::lock_guard lock(m_mutex);
                for (const auto& [id, job] : m_jobs)
                {
                    (void)id;
                    if (job == nullptr ||
                        job->state != BackgroundJobState::WaitingForDependency ||
                        job->desc.dependency.id == 0u ||
                        OwnsHandle(job->desc.dependency))
                    {
                        continue;
                    }

                    const JobHandle dependency = job->desc.dependency;
                    const bool alreadyQueued = std::any_of(
                        foregroundDependencies.begin(),
                        foregroundDependencies.end(),
                        [dependency](const JobHandle existing)
                        {
                            return existing.id == dependency.id &&
                                existing.generation == dependency.generation;
                        });
                    if (!alreadyQueued)
                        foregroundDependencies.push_back(dependency);
                }
            }

            for (const JobHandle dependency : foregroundDependencies)
                HelpForegroundDependency(dependency);
        }

        void HelpDependenciesForBackgroundJob(const JobHandle handle)
        {
            std::vector<JobHandle> foregroundDependencies;
            std::vector<JobHandle> visitedBackgroundHandles;
            {
                std::lock_guard lock(m_mutex);
                CollectForegroundDependenciesForBackgroundChainLocked(
                    handle,
                    foregroundDependencies,
                    visitedBackgroundHandles);
            }

            for (const JobHandle dependency : foregroundDependencies)
                HelpForegroundDependency(dependency);
        }

        void Complete(JobHandle handle)
        {
            if (!OwnsHandle(handle))
                return;

            if (Internal::IsExecutingBackgroundJobWorkerThread() &&
                !Completed(handle))
            {
                Internal::RecordJobViolation(
                    JobViolationKind::SyncWaitDisallowed,
                    handle.id,
                    "A background job callback cannot synchronously wait for another background job.");
                return;
            }

            while (!Completed(handle))
            {
                if (Internal::DoesJobDependencyChainContainCurrentWorker(handle))
                {
                    Internal::RecordJobViolation(
                        JobViolationKind::SyncWaitDisallowed,
                        handle.id,
                        "A job callback cannot synchronously wait for a background dependency chain that includes the currently executing job.");
                    return;
                }

                JobHandle dependency;
                {
                    std::lock_guard lock(m_mutex);
                    auto job = FindJobLocked(handle);
                    if (job == nullptr)
                        return;
                    dependency = job->desc.dependency;
                }

                if (Internal::IsCurrentJobWorkerHandle(dependency))
                {
                    Internal::RecordJobViolation(
                        JobViolationKind::SyncWaitDisallowed,
                        handle.id,
                        "A job callback cannot synchronously wait for a background job that depends on the currently executing job.");
                    return;
                }

                WakeDependencyReadyJobsForBackgroundJob(handle);
                HelpDependenciesForBackgroundJob(handle);
                std::unique_lock lock(m_mutex);
                m_workAvailable.wait_for(
                    lock,
                    std::chrono::milliseconds(1),
                    [this, handle]
                    {
                        auto job = FindJobLocked(handle);
                        return job == nullptr ||
                            job->state == BackgroundJobState::Completed ||
                            job->state == BackgroundJobState::Cancelled ||
                            job->state == BackgroundJobState::Failed ||
                            m_shutdownRequested;
                    });
            }
        }

        void Clear(JobHandle handle)
        {
            (void)handle;
        }

        void ClearContinuationsLocked()
        {
            for (const auto& continuation : m_continuations)
            {
                if (continuation.retainedExternalDependency)
                    Internal::ReleaseJobCompletionStatus(continuation.desc.dependency);
            }
            for (const auto& continuation : m_readyContinuations)
            {
                if (continuation.retainedExternalDependency)
                    Internal::ReleaseJobCompletionStatus(continuation.desc.dependency);
            }
            m_continuations.clear();
            m_readyContinuations.clear();
        }

        bool VisitForegroundDependencies(
            const JobHandle handle,
            bool (*visitor)(JobHandle dependency, void* userData),
            void* userData) const
        {
            if (visitor == nullptr)
                return false;

            JobHandle dependency;
            {
                std::lock_guard lock(m_mutex);
                auto job = FindJobLocked(handle);
                if (job == nullptr)
                    return false;

                dependency = job->desc.dependency;
                if (dependency.id == 0u || OwnsHandle(dependency))
                    return true;
            }

            return visitor(dependency, userData);
        }

        std::vector<JobHandle> CollectDependencies(const JobHandle handle) const
        {
            std::lock_guard lock(m_mutex);
            auto job = FindJobLocked(handle);
            if (job == nullptr ||
                job->state == BackgroundJobState::Completed ||
                job->state == BackgroundJobState::Cancelled ||
                job->state == BackgroundJobState::Failed)
            {
                return {};
            }

            const JobHandle dependency = job->desc.dependency;
            if (dependency.id == 0u && dependency.generation == 0u)
                return {};

            return {dependency};
        }

    private:
        std::shared_ptr<BackgroundJob> FindJobLocked(const JobHandle handle) const
        {
            if (!OwnsHandle(handle))
                return nullptr;

            const uint64_t id = handle.id & ~kBackgroundHandleBit;
            const auto found = m_jobs.find(id);
            if (found == m_jobs.end() || found->second == nullptr)
                return nullptr;

            if (found->second->generation != handle.generation)
                return nullptr;

            return found->second;
        }

        bool IsRetiredHandleLocked(const JobHandle handle) const
        {
            const uint64_t id = handle.id & ~kBackgroundHandleBit;
            const auto found = m_retiredGenerations.find(id);
            return found != m_retiredGenerations.end() && found->second == handle.generation;
        }

        JobCompletionStatus GetRetiredStatusLocked(const JobHandle handle) const
        {
            const uint64_t id = handle.id & ~kBackgroundHandleBit;
            const auto found = m_retiredStatuses.find(id);
            if (found == m_retiredStatuses.end())
                return JobCompletionStatus::Unknown;

            return found->second;
        }

        JobCompletionStatus GetJobStatusLocked(const std::shared_ptr<BackgroundJob>& job) const
        {
            if (job == nullptr)
                return JobCompletionStatus::Unknown;

            if (job->terminalCallbackRunning)
                return JobCompletionStatus::Pending;

            switch (job->state)
            {
            case BackgroundJobState::Completed:
                return JobCompletionStatus::Succeeded;
            case BackgroundJobState::Cancelled:
                return JobCompletionStatus::Cancelled;
            case BackgroundJobState::Failed:
                return JobCompletionStatus::Failed;
            default:
                return JobCompletionStatus::Pending;
            }
        }

        bool RetireJobLocked(const std::shared_ptr<BackgroundJob>& job)
        {
            if (job == nullptr)
                return false;

            m_dependencyChanged = true;
            const JobHandle dependency = job->desc.dependency;
            if (dependency.id != 0u && !OwnsHandle(dependency))
                Internal::ReleaseJobCompletionStatus(dependency);
            const JobCompletionStatus status = GetJobStatusLocked(job);
            m_retiredGenerations[job->id] = job->generation;
            m_retiredStatuses[job->id] = status;
            const bool notifyForegroundDependency =
                Internal::RecordJobTerminalStatus({kBackgroundHandleBit | job->id, job->generation}, status);
            m_retiredOrder.push_back(job->id);
            m_jobs.erase(job->id);
            PruneRetiredHandlesLocked();
            return notifyForegroundDependency;
        }

        void PruneRetiredHandlesLocked()
        {
            while (m_retiredOrder.size() > kRetiredBackgroundHandleHistoryLimit)
            {
                const uint64_t retiredId = m_retiredOrder.front();
                m_retiredOrder.pop_front();
                m_retiredGenerations.erase(retiredId);
                m_retiredStatuses.erase(retiredId);
            }
        }

        JobCompletionStatus GetDependencyStatusLocked(const JobHandle dependency) const
        {
            if (dependency.id == 0u && dependency.generation == 0u)
                return JobCompletionStatus::Succeeded;

            if (!OwnsHandle(dependency))
                return JobCompletionStatus::Unknown;

            auto job = FindJobLocked(dependency);
            if (job == nullptr)
                return IsRetiredHandleLocked(dependency)
                    ? GetRetiredStatusLocked(dependency)
                    : JobCompletionStatus::Unknown;

            return GetJobStatusLocked(job);
        }

        static void QueueTerminalCallbackLocked(
            const std::shared_ptr<BackgroundJob>& job,
            std::vector<BackgroundJobTerminalCallback>& callbacks)
        {
            if (job == nullptr || job->terminalCallbackRunning)
                return;

            job->terminalCallbackRunning = true;
            callbacks.push_back({job, job->desc.cancelFunction, job->desc.cancelUserData});
            job->desc.cancelFunction = nullptr;
            job->desc.cancelUserData = nullptr;
        }

        bool RetireJobsAfterTerminalCallbacks(
            const std::vector<BackgroundJobTerminalCallback>& callbacks)
        {
            if (callbacks.empty())
                return false;

            bool notifyForegroundDependency = false;
            bool retiredAnyJob = false;
            {
                std::lock_guard lock(m_mutex);
                for (const auto& callback : callbacks)
                {
                    const auto& job = callback.job;
                    if (job == nullptr)
                        continue;

                    const JobHandle handle{kBackgroundHandleBit | job->id, job->generation};
                    if (FindJobLocked(handle) == nullptr)
                        continue;

                    job->terminalCallbackRunning = false;
                    notifyForegroundDependency |= RetireJobLocked(job);
                    retiredAnyJob = true;
                }
            }

            if (retiredAnyJob)
                m_workAvailable.notify_all();

            return notifyForegroundDependency;
        }

        bool HasDependencyWaiterLocked() const
        {
            return std::any_of(
                m_jobs.begin(),
                m_jobs.end(),
                [](const auto& entry)
                {
                    const auto& job = entry.second;
                    return job != nullptr &&
                        job->state == BackgroundJobState::WaitingForDependency;
                });
        }

        void WakeDependencyReadyJobs()
        {
            bool wokeJob = false;
            bool notifyForegroundDependency = false;
            std::vector<BackgroundJobTerminalCallback> cancelCallbacks;
            std::vector<std::pair<JobHandle, JobCompletionStatus>> resolvedExternalDependencies;
            std::vector<JobHandle> externalDependenciesToCheck;
            {
                std::lock_guard lock(m_mutex);
                for (const auto& [id, job] : m_jobs)
                {
                    (void)id;
                    if (job == nullptr ||
                        job->state != BackgroundJobState::WaitingForDependency ||
                        job->desc.dependency.id == 0u ||
                        OwnsHandle(job->desc.dependency))
                    {
                        continue;
                    }

                    const bool alreadyQueued = std::any_of(
                        externalDependenciesToCheck.begin(),
                        externalDependenciesToCheck.end(),
                        [dependency = job->desc.dependency](const JobHandle existing)
                        {
                            return existing.id == dependency.id &&
                                existing.generation == dependency.generation;
                        });
                    if (!alreadyQueued)
                        externalDependenciesToCheck.push_back(job->desc.dependency);
                }
            }

            for (const auto dependency : externalDependenciesToCheck)
            {
                Internal::KickPendingJobForBatch(dependency);
                const auto status = Internal::GetJobCompletionStatus(dependency);
                if (status == JobCompletionStatus::Succeeded ||
                    status == JobCompletionStatus::Cancelled ||
                    status == JobCompletionStatus::Failed)
                    resolvedExternalDependencies.emplace_back(dependency, status);
            }

            {
                std::lock_guard lock(m_mutex);
                std::vector<uint64_t> waitingJobIds;
                waitingJobIds.reserve(m_jobs.size());
                for (const auto& [id, job] : m_jobs)
                {
                    if (job != nullptr &&
                        job->state == BackgroundJobState::WaitingForDependency)
                    {
                        waitingJobIds.push_back(id);
                    }
                }

                for (const uint64_t id : waitingJobIds)
                {
                    const auto found = m_jobs.find(id);
                    if (found == m_jobs.end() || found->second == nullptr)
                        continue;

                    auto job = found->second;
                    if (job->state != BackgroundJobState::WaitingForDependency)
                        continue;

                    auto dependencyStatus = GetDependencyStatusLocked(job->desc.dependency);
                    if (dependencyStatus == JobCompletionStatus::Unknown &&
                        job->desc.dependency.id != 0u &&
                        !OwnsHandle(job->desc.dependency))
                    {
                        const auto foundDependencyStatus = std::find_if(
                            resolvedExternalDependencies.begin(),
                            resolvedExternalDependencies.end(),
                            [dependency = job->desc.dependency](const std::pair<JobHandle, JobCompletionStatus>& resolved)
                            {
                                return resolved.first.id == dependency.id &&
                                    resolved.first.generation == dependency.generation;
                            });
                        if (foundDependencyStatus != resolvedExternalDependencies.end())
                            dependencyStatus = foundDependencyStatus->second;
                    }
                    if (dependencyStatus == JobCompletionStatus::Succeeded)
                    {
                        job->state = BackgroundJobState::Queued;
                        Internal::RecordJobDiagnostic(
                            kBackgroundHandleBit | job->id,
                            job->generation,
                            JobLifecycleState::Queued,
                            job->desc.debugName,
                            nullptr,
                            job->desc.dependency.id == 0u ? 0u : 1u);
                        m_readyJobs.push_back(id);
                        wokeJob = true;
                    }
                    else if (dependencyStatus == JobCompletionStatus::Cancelled ||
                        dependencyStatus == JobCompletionStatus::Failed)
                    {
                        job->state = BackgroundJobState::Cancelled;
                        Internal::RecordJobDiagnostic(
                            kBackgroundHandleBit | job->id,
                            job->generation,
                            JobLifecycleState::Cancelled,
                            job->desc.debugName,
                            nullptr,
                            job->desc.dependency.id == 0u ? 0u : 1u);
                        QueueTerminalCallbackLocked(job, cancelCallbacks);
                        wokeJob = true;
                    }
                }
            }

            for (const auto& callback : cancelCallbacks)
            {
                if (callback.job == nullptr)
                    continue;
                RunUserCallbackForJobWorker(
                    callback.function,
                    callback.userData,
                    {kBackgroundHandleBit | callback.job->id, callback.job->generation},
                    "Background job cancel callback threw an exception.");
            }
            notifyForegroundDependency |= RetireJobsAfterTerminalCallbacks(cancelCallbacks);

            if (wokeJob)
                m_workAvailable.notify_all();
            NotifyForegroundDependencyChangedIfNeeded(notifyForegroundDependency);
        }

        void WakeDependencyReadyJobsForBackgroundJob(const JobHandle handle)
        {
            std::vector<JobHandle> targetBackgroundHandles;
            {
                std::lock_guard lock(m_mutex);
                CollectBackgroundDependencyChainLocked(handle, targetBackgroundHandles);
            }

            if (targetBackgroundHandles.empty())
                return;

            std::vector<std::pair<JobHandle, JobCompletionStatus>> resolvedExternalDependencies;
            std::vector<JobHandle> externalDependenciesToCheck;
            {
                std::lock_guard lock(m_mutex);
                for (const JobHandle backgroundHandle : targetBackgroundHandles)
                {
                    auto job = FindJobLocked(backgroundHandle);
                    if (job == nullptr ||
                        job->state != BackgroundJobState::WaitingForDependency ||
                        job->desc.dependency.id == 0u ||
                        OwnsHandle(job->desc.dependency))
                    {
                        continue;
                    }

                    const bool alreadyQueued = std::any_of(
                        externalDependenciesToCheck.begin(),
                        externalDependenciesToCheck.end(),
                        [dependency = job->desc.dependency](const JobHandle existing)
                        {
                            return existing.id == dependency.id &&
                                existing.generation == dependency.generation;
                        });
                    if (!alreadyQueued)
                        externalDependenciesToCheck.push_back(job->desc.dependency);
                }
            }

            for (const JobHandle dependency : externalDependenciesToCheck)
            {
                Internal::KickPendingJobForBatch(dependency);
                const auto status = Internal::GetJobCompletionStatus(dependency);
                if (status == JobCompletionStatus::Succeeded ||
                    status == JobCompletionStatus::Cancelled ||
                    status == JobCompletionStatus::Failed)
                {
                    resolvedExternalDependencies.emplace_back(dependency, status);
                }
            }

            bool wokeJob = false;
            bool notifyForegroundDependency = false;
            std::vector<BackgroundJobTerminalCallback> cancelCallbacks;
            {
                std::lock_guard lock(m_mutex);
                for (const JobHandle backgroundHandle : targetBackgroundHandles)
                {
                    auto job = FindJobLocked(backgroundHandle);
                    if (job == nullptr || job->state != BackgroundJobState::WaitingForDependency)
                        continue;

                    auto dependencyStatus = GetDependencyStatusLocked(job->desc.dependency);
                    if (dependencyStatus == JobCompletionStatus::Unknown &&
                        job->desc.dependency.id != 0u &&
                        !OwnsHandle(job->desc.dependency))
                    {
                        const auto foundDependencyStatus = std::find_if(
                            resolvedExternalDependencies.begin(),
                            resolvedExternalDependencies.end(),
                            [dependency = job->desc.dependency](const std::pair<JobHandle, JobCompletionStatus>& resolved)
                            {
                                return resolved.first.id == dependency.id &&
                                    resolved.first.generation == dependency.generation;
                            });
                        if (foundDependencyStatus != resolvedExternalDependencies.end())
                            dependencyStatus = foundDependencyStatus->second;
                    }

                    if (dependencyStatus == JobCompletionStatus::Succeeded)
                    {
                        job->state = BackgroundJobState::Queued;
                        Internal::RecordJobDiagnostic(
                            kBackgroundHandleBit | job->id,
                            job->generation,
                            JobLifecycleState::Queued,
                            job->desc.debugName,
                            nullptr,
                            job->desc.dependency.id == 0u ? 0u : 1u);
                        m_readyJobs.push_back(job->id);
                        wokeJob = true;
                    }
                    else if (dependencyStatus == JobCompletionStatus::Cancelled ||
                        dependencyStatus == JobCompletionStatus::Failed)
                    {
                        job->state = BackgroundJobState::Cancelled;
                        Internal::RecordJobDiagnostic(
                            kBackgroundHandleBit | job->id,
                            job->generation,
                            JobLifecycleState::Cancelled,
                            job->desc.debugName,
                            nullptr,
                            job->desc.dependency.id == 0u ? 0u : 1u);
                        QueueTerminalCallbackLocked(job, cancelCallbacks);
                        wokeJob = true;
                    }
                }
            }

            for (const auto& callback : cancelCallbacks)
            {
                if (callback.job == nullptr)
                    continue;
                RunUserCallbackForJobWorker(
                    callback.function,
                    callback.userData,
                    {kBackgroundHandleBit | callback.job->id, callback.job->generation},
                    "Background job cancel callback threw an exception.");
            }
            notifyForegroundDependency |= RetireJobsAfterTerminalCallbacks(cancelCallbacks);

            if (wokeJob)
                m_workAvailable.notify_all();
            NotifyForegroundDependencyChangedIfNeeded(notifyForegroundDependency);
        }

        std::shared_ptr<BackgroundJob> PopJobLocked()
        {
            while (!m_readyJobs.empty())
            {
                const uint64_t id = m_readyJobs.front();
                m_readyJobs.pop_front();
                const auto found = m_jobs.find(id);
                if (found == m_jobs.end() || found->second == nullptr)
                    continue;

                auto job = found->second;
                if (job->state == BackgroundJobState::Queued)
                {
                    job->state = BackgroundJobState::Running;
                    return job;
                }
            }

            return nullptr;
        }

        void WorkerLoop(const uint32_t workerIndex)
        {
            const std::string threadName = "Background Job Worker " + std::to_string(workerIndex);
            NLS_PROFILE_REGISTER_THREAD(threadName.c_str());

            while (true)
            {
                std::shared_ptr<BackgroundJob> job;
                {
                    std::unique_lock lock(m_mutex);
                    if (HasDependencyWaiterLocked())
                    {
                        m_workAvailable.wait_for(
                            lock,
                            std::chrono::milliseconds(100),
                            [this]
                            {
                                return m_shutdownRequested || !m_readyJobs.empty() || m_dependencyChanged;
                            });
                        m_dependencyChanged = false;
                    }
                    else
                    {
                        m_workAvailable.wait(
                            lock,
                            [this]
                            {
                                return m_shutdownRequested || !m_readyJobs.empty() || HasDependencyWaiterLocked();
                            });
                    }

                    if (m_shutdownRequested && m_readyJobs.empty())
                        return;

                    job = PopJobLocked();
                }

                if (job == nullptr)
                {
                    WakeDependencyReadyJobs();
                    continue;
                }

                bool failed = false;
                Internal::RecordJobDiagnostic(
                    kBackgroundHandleBit | job->id,
                    job->generation,
                    JobLifecycleState::Running,
                    job->desc.debugName,
                    threadName.c_str(),
                    job->desc.dependency.id == 0u ? 0u : 1u);
                try
                {
                    NLS_PROFILE_NAMED_SCOPE(job->desc.debugName != nullptr ? job->desc.debugName : "JobSystem::BackgroundJob");
                    JobWorkerExecutionScope workerExecutionScope({kBackgroundHandleBit | job->id, job->generation});
                    job->desc.function(job->desc.userData);
                }
                catch (...)
                {
                    Internal::RecordJobViolation(
                        JobViolationKind::CallbackException,
                        kBackgroundHandleBit | job->id,
                        "Background job callback threw an exception.");
                    failed = true;
                }

                std::vector<BackgroundJobTerminalCallback> cancelCallbacks;
                bool notifyForegroundDependency = false;
                {
                    std::lock_guard lock(m_mutex);
                    if (job->state != BackgroundJobState::Cancelled)
                        job->state = failed ? BackgroundJobState::Failed : BackgroundJobState::Completed;
                    Internal::RecordJobDiagnostic(
                        kBackgroundHandleBit | job->id,
                        job->generation,
                        job->state == BackgroundJobState::Cancelled
                            ? JobLifecycleState::Cancelled
                            : (failed ? JobLifecycleState::Failed : JobLifecycleState::Completed),
                        job->desc.debugName,
                        threadName.c_str(),
                        job->desc.dependency.id == 0u ? 0u : 1u);
                    if (failed)
                    {
                        QueueTerminalCallbackLocked(job, cancelCallbacks);
                    }
                    else
                    {
                        notifyForegroundDependency |= RetireJobLocked(job);
                    }
                }
                for (const auto& callback : cancelCallbacks)
                {
                    if (callback.job == nullptr)
                        continue;
                    RunUserCallbackForJobWorker(
                        callback.function,
                        callback.userData,
                        {kBackgroundHandleBit | job->id, job->generation},
                        "Background job cancel callback threw an exception.");
                }
                notifyForegroundDependency |= RetireJobsAfterTerminalCallbacks(cancelCallbacks);
                NotifyForegroundDependencyChangedIfNeeded(notifyForegroundDependency);
                m_workAvailable.notify_all();
            }
        }

        mutable std::mutex m_mutex;
        std::condition_variable m_workAvailable;
        std::unordered_map<uint64_t, std::shared_ptr<BackgroundJob>> m_jobs;
        std::unordered_map<uint64_t, uint32_t> m_retiredGenerations;
        std::unordered_map<uint64_t, JobCompletionStatus> m_retiredStatuses;
        std::deque<uint64_t> m_retiredOrder;
        std::deque<uint64_t> m_readyJobs;
        std::deque<Continuation> m_continuations;
        std::deque<Continuation> m_readyContinuations;
        std::vector<std::thread> m_workers;
        uint64_t m_nextJobId = 1u;
        uint64_t m_nextContinuationSequence = 1u;
        uint32_t m_queueGeneration = 1u;
        bool m_acceptingWork = false;
        bool m_shutdownRequested = false;
        bool m_dependencyChanged = false;
    };

    std::mutex g_backgroundMutex;
    std::shared_ptr<BackgroundQueue> g_backgroundQueue;

    std::shared_ptr<BackgroundQueue> GetBackgroundQueue()
    {
        std::lock_guard lock(g_backgroundMutex);
        return g_backgroundQueue;
    }
}

JobHandle ScheduleBackgroundJob(const BackgroundJobDesc& desc)
{
    if (!IsJobSystemInitialized())
    {
        Internal::RecordJobViolation(
            JobViolationKind::ShutdownSchedulingRejected,
            0u,
            "Background job scheduling was rejected because the JobSystem is not accepting work.");
        return {};
    }

    auto queue = GetBackgroundQueue();
    if (queue == nullptr)
        return {};

    return queue->Schedule(desc);
}

bool ScheduleMainThreadContinuation(const MainThreadContinuationDesc& desc)
{
    if (!IsJobSystemInitialized())
    {
        Internal::RecordJobViolation(
            JobViolationKind::ShutdownSchedulingRejected,
            0u,
            "Main-thread continuation scheduling was rejected because the JobSystem is not accepting work.");
        return false;
    }

    auto queue = GetBackgroundQueue();
    return queue != nullptr && queue->ScheduleContinuation(desc);
}

uint32_t DrainMainThreadContinuations(const uint32_t maxContinuations)
{
    auto queue = GetBackgroundQueue();
    if (queue == nullptr)
        return 0u;

    return queue->DrainContinuations(maxContinuations);
}

namespace Internal
{
    bool StartBackgroundJobQueue(const uint32_t workerCount)
    {
        {
            std::lock_guard lock(g_backgroundMutex);
            g_backgroundQueue.reset();
        }

        auto queue = std::make_shared<BackgroundQueue>();
        if (!queue->Start(workerCount))
        {
            std::lock_guard lock(g_backgroundMutex);
            g_backgroundQueue.reset();
            return false;
        }

        {
            std::lock_guard lock(g_backgroundMutex);
            g_backgroundQueue = std::move(queue);
        }
        return true;
    }

    void ShutdownBackgroundJobQueue(const JobSystemShutdownMode mode)
    {
        std::shared_ptr<BackgroundQueue> queue;
        {
            std::lock_guard lock(g_backgroundMutex);
            queue = g_backgroundQueue;
        }

        if (queue != nullptr)
            queue->Shutdown(mode);
    }

    void StopAcceptingBackgroundJobQueue()
    {
        auto queue = GetBackgroundQueue();
        if (queue != nullptr)
            queue->StopAcceptingWork();
    }

    bool IsBackgroundJobHandle(const JobHandle handle)
    {
        return (handle.id & kBackgroundHandleBit) != 0u;
    }

    bool IsKnownBackgroundJobHandle(const JobHandle handle)
    {
        auto queue = GetBackgroundQueue();
        return queue != nullptr && queue->Known(handle);
    }

    bool IsBackgroundJobCompleted(const JobHandle handle)
    {
        auto queue = GetBackgroundQueue();
        if (queue != nullptr)
            return queue->Completed(handle);

        const auto status = NLS::Base::Jobs::Internal::GetJobCompletionStatus(handle);
        return status == JobCompletionStatus::Succeeded ||
            status == JobCompletionStatus::Cancelled ||
            status == JobCompletionStatus::Failed;
    }

    JobCompletionStatus GetBackgroundJobCompletionStatus(const JobHandle handle)
    {
        auto queue = GetBackgroundQueue();
        if (queue == nullptr)
            return JobCompletionStatus::Unknown;

        return queue->CompletionStatus(handle);
    }

    bool VisitForegroundDependenciesForBackgroundJob(
        const JobHandle handle,
        bool (*visitor)(JobHandle dependency, void* userData),
        void* userData)
    {
        auto queue = GetBackgroundQueue();
        if (queue == nullptr)
            return false;

        return queue->VisitForegroundDependencies(handle, visitor, userData);
    }

    std::vector<JobHandle> CollectBackgroundJobDependencies(const JobHandle handle)
    {
        auto queue = GetBackgroundQueue();
        if (queue == nullptr)
            return {};

        return queue->CollectDependencies(handle);
    }

    void NotifyBackgroundDependencyChanged()
    {
        auto queue = GetBackgroundQueue();
        if (queue != nullptr)
            queue->NotifyDependencyChanged();
    }

    void CompleteBackgroundJob(const JobHandle handle)
    {
        auto queue = GetBackgroundQueue();
        if (queue != nullptr)
            queue->Complete(handle);
    }

    void ClearBackgroundJob(const JobHandle handle)
    {
        auto queue = GetBackgroundQueue();
        if (queue != nullptr)
            queue->Clear(handle);
    }

    void ClearBackgroundJobRetiredHistory()
    {
        auto queue = GetBackgroundQueue();
        if (queue != nullptr)
            queue->ClearRetiredHistory();
    }
}
}
