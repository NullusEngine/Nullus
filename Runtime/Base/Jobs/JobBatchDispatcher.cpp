#include "Jobs/JobBatchDispatcher.h"

#include "Jobs/JobSystem.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace NLS::Base::Jobs
{
    JobBatchDispatcher::JobBatchDispatcher(const int jobsPerBatch)
        : m_jobsPerBatch(jobsPerBatch)
    {
    }

    JobBatchDispatcher::~JobBatchDispatcher()
    {
        Kick();
    }

    JobHandle JobBatchDispatcher::AddJob(const JobScheduleDesc& desc)
    {
        auto handle = Internal::CreatePendingJobForBatch(desc);
        if (handle.id != 0u)
        {
            m_pendingHandles.push_back(handle);
            ++m_pendingJobCount;
            const int jobsPerBatch = ResolveJobsPerBatch();
            if (jobsPerBatch != kManualJobKick &&
                m_pendingJobCount >= static_cast<size_t>(jobsPerBatch))
            {
                Kick();
            }
        }
        return handle;
    }

    JobHandle JobBatchDispatcher::AddForEach(const JobForEachDesc& desc)
    {
        auto handle = Internal::CreatePendingForEachJobForBatch(desc);
        if (handle.id != 0u)
        {
            m_pendingHandles.push_back(handle);
            m_pendingJobCount += static_cast<size_t>(std::max(1u, desc.iterationCount));
            const int jobsPerBatch = ResolveJobsPerBatch();
            if (jobsPerBatch != kManualJobKick &&
                m_pendingJobCount >= static_cast<size_t>(jobsPerBatch))
            {
                Kick();
            }
        }
        return handle;
    }

    JobHandle JobBatchDispatcher::AddMultiDependency(
        const JobHandle* dependencies,
        const size_t dependencyCount)
    {
        return ScheduleMultiDependencyJob(*this, dependencies, dependencyCount);
    }

    void JobBatchDispatcher::Kick()
    {
        for (const auto handle : m_pendingHandles)
            Internal::KickPendingJobForBatch(handle);
        m_pendingHandles.clear();
        m_pendingJobCount = 0u;
    }

    bool JobBatchDispatcher::Empty() const
    {
        return m_pendingHandles.empty();
    }

    int JobBatchDispatcher::ResolveJobsPerBatch()
    {
        if (m_jobsPerBatch == kBatchKickByWorkerCount)
            return static_cast<int>(std::max(1u, GetJobWorkerCount()));

        if (m_jobsPerBatch == kManualJobKick)
            return kManualJobKick;

        return std::max(1, m_jobsPerBatch);
    }

    JobHandle ScheduleMultiDependencyJob(
        JobBatchDispatcher& dispatcher,
        const JobHandle* dependencies,
        const size_t dependencyCount)
    {
        if (dependencies == nullptr || dependencyCount == 0u)
            return {};

        for (size_t index = 0u; index < dependencyCount; ++index)
        {
            if (!Internal::IsKnownJobHandle(dependencies[index]))
                return {};
        }

        bool allSame = true;
        for (size_t index = 1u; index < dependencyCount; ++index)
        {
            if (dependencies[index] != dependencies[0])
            {
                allSame = false;
                break;
            }
        }

        dispatcher.Kick();
        if (allSame)
            return dependencies[0];

        return Internal::ScheduleMultiDependencyFence(dependencies, dependencyCount);
    }
}
