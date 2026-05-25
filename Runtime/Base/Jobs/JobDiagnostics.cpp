#include "Jobs/JobDiagnostics.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace NLS::Base::Jobs
{
namespace
{
    constexpr size_t kMaxRecentJobRecords = 256u;
    constexpr size_t kMaxRecentViolationRecords = 128u;
    static_assert(
        static_cast<uint32_t>(JobLifecycleState::Count) == 7u,
        "Update diagnostics counters when lifecycle states change.");

    std::mutex g_diagnosticsMutex;
    JobDiagnosticSnapshot g_snapshot;
    std::atomic<bool> g_diagnosticsEnabled{false};

    struct JobDiagnosticKey
    {
        uint64_t id = 0u;
        uint32_t generation = 0u;

        bool operator==(const JobDiagnosticKey& other) const
        {
            return id == other.id && generation == other.generation;
        }
    };

    struct JobDiagnosticKeyHash
    {
        size_t operator()(const JobDiagnosticKey& key) const
        {
            const auto idHash = std::hash<uint64_t>{}(key.id);
            const auto generationHash = std::hash<uint32_t>{}(key.generation);
            return idHash ^ (generationHash + 0x9e3779b97f4a7c15ull + (idHash << 6u) + (idHash >> 2u));
        }
    };

    std::unordered_map<JobDiagnosticKey, JobLifecycleState, JobDiagnosticKeyHash> g_countedJobStates;

    std::string MakeDefaultThreadName()
    {
        return "Thread " + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    template<typename RecordT>
    void PushBounded(std::vector<RecordT>& records, RecordT record, const size_t limit)
    {
        if (records.size() >= limit)
        {
            records.erase(records.begin());
            ++g_snapshot.droppedHistoryCount;
        }

        records.push_back(std::move(record));
    }
}

JobDiagnosticSnapshot CopyJobDiagnosticSnapshot()
{
    std::lock_guard lock(g_diagnosticsMutex);
    return g_snapshot;
}

    void ResetJobDiagnosticsForTesting()
    {
        std::lock_guard lock(g_diagnosticsMutex);
        g_snapshot = {};
        g_countedJobStates.clear();
        g_diagnosticsEnabled.store(false, std::memory_order_release);
    }

namespace Internal
{
    void SetJobDiagnosticsRuntimeState(
        const bool initialized,
        const uint32_t workerCount,
        const bool enabled)
    {
        std::lock_guard lock(g_diagnosticsMutex);
        g_diagnosticsEnabled.store(enabled, std::memory_order_release);
        g_snapshot.initialized = initialized;
        g_snapshot.workerCount = workerCount;
        if (!initialized)
        {
            g_snapshot.queuedJobCount = 0u;
            g_snapshot.runningJobCount = 0u;
            g_countedJobStates.clear();
        }
    }

    bool AreJobDiagnosticsEnabled()
    {
        return g_diagnosticsEnabled.load(std::memory_order_acquire);
    }

    void RecordJobDiagnostic(
        const uint64_t id,
        const uint32_t generation,
        const JobLifecycleState state,
        const char* debugName,
        const char* workerName,
        const uint64_t dependencyCount)
    {
        if (!g_diagnosticsEnabled.load(std::memory_order_acquire))
            return;

        std::lock_guard lock(g_diagnosticsMutex);
        auto migrateFromPreviousState = [](const JobLifecycleState previous)
        {
            if (previous == JobLifecycleState::Queued && g_snapshot.queuedJobCount > 0u)
                --g_snapshot.queuedJobCount;
            else if (previous == JobLifecycleState::Running && g_snapshot.runningJobCount > 0u)
                --g_snapshot.runningJobCount;
        };

        const JobDiagnosticKey key{id, generation};
        const auto previousIter = g_countedJobStates.find(key);
        const auto previousState = previousIter != g_countedJobStates.end()
            ? previousIter->second
            : JobLifecycleState::Count;
        switch (state)
        {
        case JobLifecycleState::Created:
        case JobLifecycleState::WaitingForDependencies:
            break;
        case JobLifecycleState::Queued:
            if (previousState != JobLifecycleState::Queued)
            {
                migrateFromPreviousState(previousState);
                ++g_snapshot.queuedJobCount;
            }
            g_countedJobStates[key] = state;
            break;
        case JobLifecycleState::Running:
            if (previousState != JobLifecycleState::Running)
            {
                migrateFromPreviousState(previousState);
                ++g_snapshot.runningJobCount;
            }
            g_countedJobStates[key] = state;
            break;
        case JobLifecycleState::Completed:
            if (previousState != JobLifecycleState::Completed)
            {
                migrateFromPreviousState(previousState);
                ++g_snapshot.completedJobCount;
            }
            g_countedJobStates[key] = state;
            break;
        case JobLifecycleState::Failed:
            if (previousState != JobLifecycleState::Failed)
            {
                migrateFromPreviousState(previousState);
                ++g_snapshot.failedJobCount;
            }
            g_countedJobStates[key] = state;
            break;
        case JobLifecycleState::Cancelled:
            if (previousState != JobLifecycleState::Cancelled)
                migrateFromPreviousState(previousState);
            g_countedJobStates[key] = state;
            break;
        case JobLifecycleState::Count:
            break;
        }

        JobDiagnosticRecord record;
        record.id = id;
        record.generation = generation;
        record.state = state;
        record.debugName = debugName != nullptr ? debugName : "";
        record.workerName = workerName != nullptr ? workerName : "";
        record.dependencyCount = dependencyCount;
        PushBounded(g_snapshot.recentJobs, std::move(record), kMaxRecentJobRecords);
    }

    void RecordJobViolation(
        const JobViolationKind kind,
        const uint64_t jobId,
        const char* message,
        const char* threadName)
    {
        if (!g_diagnosticsEnabled.load(std::memory_order_acquire))
            return;

        std::lock_guard lock(g_diagnosticsMutex);
        JobViolationRecord record;
        record.kind = kind;
        record.jobId = jobId;
        record.message = message != nullptr ? message : "";
        record.threadName = threadName != nullptr ? threadName : MakeDefaultThreadName();
        PushBounded(g_snapshot.recentViolations, std::move(record), kMaxRecentViolationRecords);
    }
}
}
