#include "Jobs/JobBindings.h"

#include "Jobs/JobSystem.h"

namespace
{
    constexpr uint32_t kBindingVersion = NLS_JOB_BINDING_VERSION;
    static_assert(NLS_JOB_PRIORITY_NORMAL == 0, "C ABI priority constants are part of the stable binding contract.");
    static_assert(NLS_JOB_PRIORITY_HIGH == 1, "C ABI priority constants are part of the stable binding contract.");
    static_assert(NLS_JOB_SAFETY_MAY_SYNC_WAIT == 0, "C ABI safety constants are part of the stable binding contract.");
    static_assert(
        NLS_JOB_SAFETY_GUARANTEED_NO_SYNC_WAIT == 1,
        "C ABI safety constants are part of the stable binding contract.");
    static_assert(
        static_cast<uint32_t>(NLS::Base::Jobs::JobCompletionStatus::Count) == 5u,
        "Update C binding completion status mapping when native completion statuses change.");

    NLS::Base::Jobs::JobHandle ToNativeHandle(const NLS_BindingJobHandle& handle)
    {
        return {handle.id, handle.generation};
    }

    void FromNativeHandle(const NLS::Base::Jobs::JobHandle handle, NLS_BindingJobHandle& outHandle)
    {
        outHandle.structSize = sizeof(NLS_BindingJobHandle);
        outHandle.version = kBindingVersion;
        outHandle.id = handle.id;
        outHandle.generation = handle.generation;
        outHandle.reserved = 0u;
    }

    bool IsDefaultHandle(const NLS_BindingJobHandle& handle)
    {
        return handle.id == 0u && handle.generation == 0u;
    }

    bool IsAllZeroHandle(const NLS_BindingJobHandle& handle)
    {
        return handle.structSize == 0u &&
            handle.version == 0u &&
            handle.id == 0u &&
            handle.generation == 0u &&
            handle.reserved == 0u;
    }

    NLS_JobStatusCode ValidateHandleHeader(const NLS_BindingJobHandle& handle)
    {
        if (IsAllZeroHandle(handle))
            return NLS_JOB_STATUS_OK;

        if (handle.structSize != sizeof(NLS_BindingJobHandle) || handle.version != kBindingVersion)
            return NLS_JOB_STATUS_VERSION_MISMATCH;

        return NLS_JOB_STATUS_OK;
    }

    NLS_JobStatusCode ValidateDependency(const NLS_BindingJobHandle& handle)
    {
        const auto status = ValidateHandleHeader(handle);
        if (status != NLS_JOB_STATUS_OK)
            return status;

        if (!IsDefaultHandle(handle) &&
            !NLS::Base::Jobs::Internal::IsKnownJobHandle(ToNativeHandle(handle)))
        {
            return NLS_JOB_STATUS_INVALID_HANDLE;
        }

        return NLS_JOB_STATUS_OK;
    }

    bool IsValidPriority(const uint32_t priority)
    {
        return priority == NLS_JOB_PRIORITY_NORMAL || priority == NLS_JOB_PRIORITY_HIGH;
    }

    bool IsValidSafetyPolicy(const uint32_t safetyPolicy)
    {
        return safetyPolicy == NLS_JOB_SAFETY_MAY_SYNC_WAIT ||
            safetyPolicy == NLS_JOB_SAFETY_GUARANTEED_NO_SYNC_WAIT;
    }

    NLS::Base::Jobs::JobPriority ToPriority(const uint32_t priority)
    {
        return priority == NLS_JOB_PRIORITY_HIGH
            ? NLS::Base::Jobs::JobPriority::High
            : NLS::Base::Jobs::JobPriority::Normal;
    }

    NLS::Base::Jobs::JobSafetyPolicy ToSafetyPolicy(const uint32_t safetyPolicy)
    {
        return safetyPolicy == NLS_JOB_SAFETY_GUARANTEED_NO_SYNC_WAIT
            ? NLS::Base::Jobs::JobSafetyPolicy::GuaranteedNoSyncWait
            : NLS::Base::Jobs::JobSafetyPolicy::MaySyncWait;
    }

    NLS_JobStatusCode ToBindingCompletionStatus(const NLS::Base::Jobs::JobCompletionStatus status)
    {
        switch (status)
        {
        case NLS::Base::Jobs::JobCompletionStatus::Succeeded:
            return NLS_JOB_STATUS_OK;
        case NLS::Base::Jobs::JobCompletionStatus::Cancelled:
            return NLS_JOB_STATUS_CANCELLED;
        case NLS::Base::Jobs::JobCompletionStatus::Failed:
        case NLS::Base::Jobs::JobCompletionStatus::Pending:
            return NLS_JOB_STATUS_FAILED;
        case NLS::Base::Jobs::JobCompletionStatus::Unknown:
            return NLS_JOB_STATUS_INVALID_HANDLE;
        case NLS::Base::Jobs::JobCompletionStatus::Count:
            break;
        }
        return NLS_JOB_STATUS_INVALID_HANDLE;
    }

    bool IsTerminalCompletionStatus(const NLS::Base::Jobs::JobCompletionStatus status)
    {
        return status == NLS::Base::Jobs::JobCompletionStatus::Succeeded ||
            status == NLS::Base::Jobs::JobCompletionStatus::Cancelled ||
            status == NLS::Base::Jobs::JobCompletionStatus::Failed;
    }
}

NLS_JobStatusCode NLS_Jobs_GetWorkerCount(uint32_t* outWorkerCount)
{
    if (outWorkerCount == nullptr)
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    if (!NLS::Base::Jobs::IsJobSystemInitialized())
        return NLS_JOB_STATUS_NOT_INITIALIZED;

    *outWorkerCount = NLS::Base::Jobs::GetJobWorkerCount();
    return NLS_JOB_STATUS_OK;
}

NLS_JobStatusCode NLS_Jobs_Schedule(
    const NLS_BindingJobScheduleDesc* desc,
    NLS_BindingJobHandle* outHandle)
{
    if (desc == nullptr || outHandle == nullptr)
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    if (desc->structSize != sizeof(NLS_BindingJobScheduleDesc) || desc->version != kBindingVersion)
        return NLS_JOB_STATUS_VERSION_MISMATCH;

    if (desc->callback == nullptr)
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    if (!IsValidPriority(desc->priority) || !IsValidSafetyPolicy(desc->safetyPolicy))
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    if (!NLS::Base::Jobs::IsJobSystemInitialized())
        return NLS_JOB_STATUS_NOT_INITIALIZED;

    const auto dependencyStatus = ValidateDependency(desc->dependency);
    if (dependencyStatus != NLS_JOB_STATUS_OK)
        return dependencyStatus;

    NLS::Base::Jobs::JobScheduleDesc nativeDesc;
    nativeDesc.function = desc->callback;
    nativeDesc.userData = desc->userData;
    nativeDesc.dependency = ToNativeHandle(desc->dependency);
    nativeDesc.priority = ToPriority(desc->priority);
    nativeDesc.safetyPolicy = ToSafetyPolicy(desc->safetyPolicy);
    nativeDesc.debugName = desc->debugName;

    const auto handle = NLS::Base::Jobs::ScheduleJob(nativeDesc);
    if (handle.id == 0u)
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    FromNativeHandle(handle, *outHandle);
    return NLS_JOB_STATUS_OK;
}

NLS_JobStatusCode NLS_Jobs_ScheduleForEach(
    const NLS_BindingForEachScheduleDesc* desc,
    NLS_BindingJobHandle* outHandle)
{
    if (desc == nullptr || outHandle == nullptr)
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    if (desc->structSize != sizeof(NLS_BindingForEachScheduleDesc) || desc->version != kBindingVersion)
        return NLS_JOB_STATUS_VERSION_MISMATCH;

    if (desc->callback == nullptr)
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    if (!IsValidPriority(desc->priority) || !IsValidSafetyPolicy(desc->safetyPolicy))
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    if (!NLS::Base::Jobs::IsJobSystemInitialized())
        return NLS_JOB_STATUS_NOT_INITIALIZED;

    const auto dependencyStatus = ValidateDependency(desc->dependency);
    if (dependencyStatus != NLS_JOB_STATUS_OK)
        return dependencyStatus;

    if (desc->iterationCount == 0u)
    {
        FromNativeHandle({}, *outHandle);
        return NLS_JOB_STATUS_OK;
    }

    NLS::Base::Jobs::JobForEachDesc nativeDesc;
    nativeDesc.function = desc->callback;
    nativeDesc.userData = desc->userData;
    nativeDesc.iterationCount = desc->iterationCount;
    nativeDesc.batchSize = desc->batchSize == 0u ? 1u : desc->batchSize;
    nativeDesc.combineFunction = desc->combineCallback;
    nativeDesc.dependency = ToNativeHandle(desc->dependency);
    nativeDesc.priority = ToPriority(desc->priority);
    nativeDesc.safetyPolicy = ToSafetyPolicy(desc->safetyPolicy);
    nativeDesc.debugName = desc->debugName;

    const auto handle = NLS::Base::Jobs::ScheduleJobForEach(nativeDesc);
    if (handle.id == 0u)
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    FromNativeHandle(handle, *outHandle);
    return NLS_JOB_STATUS_OK;
}

NLS_JobStatusCode NLS_Jobs_IsCompleted(
    const NLS_BindingJobHandle* handle,
    uint32_t* outCompleted)
{
    if (handle == nullptr || outCompleted == nullptr)
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    const auto status = ValidateHandleHeader(*handle);
    if (status != NLS_JOB_STATUS_OK)
        return status;

    if (!IsDefaultHandle(*handle) && !NLS::Base::Jobs::IsJobSystemInitialized())
        return NLS_JOB_STATUS_NOT_INITIALIZED;

    if (!IsDefaultHandle(*handle) &&
        !NLS::Base::Jobs::Internal::IsKnownJobHandle(ToNativeHandle(*handle)))
    {
        return NLS_JOB_STATUS_INVALID_HANDLE;
    }

    *outCompleted = NLS::Base::Jobs::IsCompleted(ToNativeHandle(*handle)) ? 1u : 0u;
    return NLS_JOB_STATUS_OK;
}

NLS_JobStatusCode NLS_Jobs_Complete(NLS_BindingJobHandle* handle)
{
    if (handle == nullptr)
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    const auto status = ValidateHandleHeader(*handle);
    if (status != NLS_JOB_STATUS_OK)
        return status;

    if (IsDefaultHandle(*handle))
        return NLS_JOB_STATUS_OK;

    if (!NLS::Base::Jobs::IsJobSystemInitialized())
        return NLS_JOB_STATUS_NOT_INITIALIZED;

    auto nativeHandle = ToNativeHandle(*handle);
    if (!NLS::Base::Jobs::Internal::IsKnownJobHandle(nativeHandle))
        return NLS_JOB_STATUS_INVALID_HANDLE;

    NLS::Base::Jobs::CompleteNoClear(nativeHandle);
    const auto completionStatus = NLS::Base::Jobs::Internal::GetJobCompletionStatus(nativeHandle);
    if (IsTerminalCompletionStatus(completionStatus))
    {
        nativeHandle = {};
        FromNativeHandle(nativeHandle, *handle);
    }
    return ToBindingCompletionStatus(completionStatus);
}

NLS_JobStatusCode NLS_Jobs_ClearWithoutSync(NLS_BindingJobHandle* handle)
{
    if (handle == nullptr)
        return NLS_JOB_STATUS_INVALID_ARGUMENT;

    const auto status = ValidateHandleHeader(*handle);
    if (status != NLS_JOB_STATUS_OK)
        return status;

    if (IsDefaultHandle(*handle))
        return NLS_JOB_STATUS_OK;

    if (!NLS::Base::Jobs::IsJobSystemInitialized())
        return NLS_JOB_STATUS_NOT_INITIALIZED;

    auto nativeHandle = ToNativeHandle(*handle);
    if (!NLS::Base::Jobs::Internal::IsKnownJobHandle(nativeHandle))
        return NLS_JOB_STATUS_INVALID_HANDLE;

    NLS::Base::Jobs::ClearWithoutSync(nativeHandle);
    FromNativeHandle(nativeHandle, *handle);
    return NLS_JOB_STATUS_OK;
}
