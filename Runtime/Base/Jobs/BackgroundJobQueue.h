#pragma once

#include "Jobs/JobTypes.h"

#include <vector>

namespace NLS::Base::Jobs
{
    using MainThreadContinuationFunction = void(*)(void* userData);

    struct NLS_BASE_API BackgroundJobDesc
    {
        JobFunction function = nullptr;
        void* userData = nullptr;
        JobFunction cancelFunction = nullptr;
        void* cancelUserData = nullptr;
        JobHandle dependency;
        const char* debugName = nullptr;
    };

    struct NLS_BASE_API MainThreadContinuationDesc
    {
        MainThreadContinuationFunction function = nullptr;
        void* userData = nullptr;
        JobHandle dependency;
        const char* debugName = nullptr;
    };

    NLS_BASE_API JobHandle ScheduleBackgroundJob(const BackgroundJobDesc& desc);
    NLS_BASE_API bool ScheduleMainThreadContinuation(const MainThreadContinuationDesc& desc);
    NLS_BASE_API uint32_t DrainMainThreadContinuations(uint32_t maxContinuations = 0u);

    namespace Internal
    {
        NLS_BASE_API bool StartBackgroundJobQueue(uint32_t workerCount);
        NLS_BASE_API void StopAcceptingBackgroundJobQueue();
        NLS_BASE_API void ShutdownBackgroundJobQueue(JobSystemShutdownMode mode);
        NLS_BASE_API bool IsBackgroundJobHandle(JobHandle handle);
        NLS_BASE_API bool IsKnownBackgroundJobHandle(JobHandle handle);
        NLS_BASE_API bool IsBackgroundJobCompleted(JobHandle handle);
        NLS_BASE_API JobCompletionStatus GetBackgroundJobCompletionStatus(JobHandle handle);
        NLS_BASE_API bool VisitForegroundDependenciesForBackgroundJob(
            JobHandle handle,
            bool (*visitor)(JobHandle dependency, void* userData),
            void* userData);
        NLS_BASE_API std::vector<JobHandle> CollectBackgroundJobDependencies(JobHandle handle);
        NLS_BASE_API void NotifyBackgroundDependencyChanged();
        NLS_BASE_API void CompleteBackgroundJob(JobHandle handle);
        NLS_BASE_API void ClearBackgroundJob(JobHandle handle);
        NLS_BASE_API void ClearBackgroundJobRetiredHistory();
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS_BASE_API bool HasBackgroundJobQueueForTesting();
        NLS_BASE_API void ResetBackgroundJobQueueForTesting();
#endif
    }
}
