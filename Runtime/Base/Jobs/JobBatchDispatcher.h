#pragma once

#include "Jobs/JobTypes.h"

#include <vector>

namespace NLS::Base::Jobs
{
    class NLS_BASE_API JobBatchDispatcher
    {
    public:
        // Caller-owned batching helper. Serialize access externally when sharing a dispatcher across threads.
        static constexpr int kManualJobKick = -1;
        static constexpr int kBatchKickByWorkerCount = -2;

        explicit JobBatchDispatcher(int jobsPerBatch = kManualJobKick);
        ~JobBatchDispatcher();

        JobHandle AddJob(const JobScheduleDesc& desc);
        JobHandle AddForEach(const JobForEachDesc& desc);
        JobHandle AddMultiDependency(const JobHandle* dependencies, size_t dependencyCount);
        void Kick();
        bool Empty() const;

    private:
        int ResolveJobsPerBatch();

        std::vector<JobHandle> m_pendingHandles;
        int m_jobsPerBatch = kManualJobKick;
        size_t m_pendingJobCount = 0u;
    };

    NLS_BASE_API JobHandle ScheduleMultiDependencyJob(
        JobBatchDispatcher& dispatcher,
        const JobHandle* dependencies,
        size_t dependencyCount);
}
