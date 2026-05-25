#pragma once

#include "BaseDef.h"

#include <cstdint>
#include <string>
#include <vector>

namespace NLS::Base::Jobs
{
    enum class JobLifecycleState : uint8_t
    {
        Created = 0,
        WaitingForDependencies,
        Queued,
        Running,
        Completed,
        Cancelled,
        Failed,
        Count
    };

    enum class JobViolationKind : uint8_t
    {
        SyncWaitDisallowed = 0,
        InvalidHandle,
        NullCallback,
        ShutdownSchedulingRejected,
        StaleHandle,
        ClearedWithoutSync,
        CallbackException
    };

    struct NLS_BASE_API JobDiagnosticRecord
    {
        uint64_t id = 0u;
        uint32_t generation = 0u;
        JobLifecycleState state = JobLifecycleState::Created;
        std::string debugName;
        std::string workerName;
        uint64_t dependencyCount = 0u;
    };

    struct NLS_BASE_API JobViolationRecord
    {
        JobViolationKind kind = JobViolationKind::InvalidHandle;
        uint64_t jobId = 0u;
        std::string message;
        std::string threadName;
    };

    struct NLS_BASE_API JobDiagnosticSnapshot
    {
        bool initialized = false;
        uint32_t workerCount = 0u;
        uint64_t queuedJobCount = 0u;
        uint64_t runningJobCount = 0u;
        uint64_t completedJobCount = 0u;
        uint64_t failedJobCount = 0u;
        uint64_t droppedHistoryCount = 0u;
        std::vector<JobDiagnosticRecord> recentJobs;
        std::vector<JobViolationRecord> recentViolations;
    };

    NLS_BASE_API JobDiagnosticSnapshot CopyJobDiagnosticSnapshot();
    NLS_BASE_API void ResetJobDiagnosticsForTesting();

    namespace Internal
    {
        NLS_BASE_API void SetJobDiagnosticsRuntimeState(bool initialized, uint32_t workerCount, bool enabled);
        NLS_BASE_API bool AreJobDiagnosticsEnabled();
        NLS_BASE_API void RecordJobDiagnostic(
            uint64_t id,
            uint32_t generation,
            JobLifecycleState state,
            const char* debugName,
            const char* workerName,
            uint64_t dependencyCount);
        NLS_BASE_API void RecordJobViolation(
            JobViolationKind kind,
            uint64_t jobId,
            const char* message,
            const char* threadName = nullptr);
    }
}
