#pragma once

#include "BaseDef.h"

#include <cstddef>
#include <cstdint>

namespace NLS::Base::Jobs
{
    inline constexpr uint32_t kAutoJobWorkerCount = 0xffffffffu;

    struct NLS_BASE_API JobHandle
    {
        uint64_t id = 0u;
        uint32_t generation = 0u;
    };

    inline bool operator==(const JobHandle& lhs, const JobHandle& rhs)
    {
        return lhs.id == rhs.id && lhs.generation == rhs.generation;
    }

    inline bool operator!=(const JobHandle& lhs, const JobHandle& rhs)
    {
        return !(lhs == rhs);
    }

    enum class JobPriority : uint8_t
    {
        Normal = 0,
        High
    };

    enum class JobSafetyPolicy : uint8_t
    {
        MaySyncWait = 0,
        GuaranteedNoSyncWait
    };

    enum class JobSystemShutdownMode : uint8_t
    {
        DrainAcceptedWork = 0,
        Immediate
    };

    enum class JobCompletionStatus : uint8_t
    {
        Unknown = 0,
        Pending,
        Succeeded,
        Cancelled,
        Failed,
        Count
    };

    struct NLS_BASE_API JobSystemConfig
    {
        uint32_t workerCount = kAutoJobWorkerCount;
        uint32_t backgroundWorkerCount = 1u;
        bool enableDiagnostics = false;
    };

    using JobFunction = void(*)(void* userData);
    using JobForEachFunction = void(*)(void* userData, uint32_t index);

    struct NLS_BASE_API JobScheduleDesc
    {
        JobFunction function = nullptr;
        void* userData = nullptr;
        JobFunction cancelFunction = nullptr;
        void* cancelUserData = nullptr;
        JobHandle dependency;
        JobPriority priority = JobPriority::Normal;
        JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait;
        const char* debugName = nullptr;
    };

    struct NLS_BASE_API JobForEachDesc
    {
        JobForEachFunction function = nullptr;
        void* userData = nullptr;
        uint32_t iterationCount = 0u;
        uint32_t batchSize = 1u;
        JobFunction combineFunction = nullptr;
        JobFunction cancelFunction = nullptr;
        JobHandle dependency;
        JobPriority priority = JobPriority::Normal;
        JobSafetyPolicy safetyPolicy = JobSafetyPolicy::MaySyncWait;
        const char* debugName = nullptr;
    };
}
