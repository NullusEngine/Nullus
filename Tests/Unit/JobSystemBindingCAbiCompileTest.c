#include "Jobs/JobBindings.h"

#include <stdint.h>
#include <stddef.h>

#define NLS_CABI_STATIC_ASSERT(condition, name) typedef char name[(condition) ? 1 : -1]

NLS_CABI_STATIC_ASSERT(NLS_JOB_BINDING_VERSION == 1u, nls_job_binding_version_changed);
NLS_CABI_STATIC_ASSERT(NLS_JOB_STATUS_OK == 0, nls_job_status_ok_layout_changed);
NLS_CABI_STATIC_ASSERT(NLS_JOB_STATUS_NOT_INITIALIZED == 1, nls_job_status_not_initialized_layout_changed);
NLS_CABI_STATIC_ASSERT(NLS_JOB_STATUS_INVALID_ARGUMENT == 2, nls_job_status_invalid_argument_layout_changed);
NLS_CABI_STATIC_ASSERT(NLS_JOB_STATUS_INVALID_HANDLE == 3, nls_job_status_invalid_handle_layout_changed);
NLS_CABI_STATIC_ASSERT(NLS_JOB_STATUS_VERSION_MISMATCH == 4, nls_job_status_version_mismatch_layout_changed);
NLS_CABI_STATIC_ASSERT(NLS_JOB_STATUS_CANCELLED == 5, nls_job_status_cancelled_layout_changed);
NLS_CABI_STATIC_ASSERT(NLS_JOB_STATUS_FAILED == 6, nls_job_status_failed_layout_changed);
NLS_CABI_STATIC_ASSERT(NLS_JOB_PRIORITY_NORMAL == 0, nls_job_priority_normal_layout_changed);
NLS_CABI_STATIC_ASSERT(NLS_JOB_PRIORITY_HIGH == 1, nls_job_priority_high_layout_changed);
NLS_CABI_STATIC_ASSERT(NLS_JOB_SAFETY_MAY_SYNC_WAIT == 0, nls_job_safety_may_sync_wait_layout_changed);
NLS_CABI_STATIC_ASSERT(
    NLS_JOB_SAFETY_GUARANTEED_NO_SYNC_WAIT == 1,
    nls_job_safety_guaranteed_no_sync_wait_layout_changed);
NLS_CABI_STATIC_ASSERT(offsetof(NLS_BindingJobHandle, structSize) == 0u, nls_job_handle_struct_size_offset_changed);
NLS_CABI_STATIC_ASSERT(offsetof(NLS_BindingJobHandle, id) == 8u, nls_job_handle_id_offset_changed);
NLS_CABI_STATIC_ASSERT(
    offsetof(NLS_BindingJobScheduleDesc, callback) == 8u,
    nls_job_schedule_callback_offset_changed);
NLS_CABI_STATIC_ASSERT(
    offsetof(NLS_BindingForEachScheduleDesc, callback) == 8u,
    nls_job_for_each_schedule_callback_offset_changed);

#if UINTPTR_MAX == UINT64_MAX
    NLS_CABI_STATIC_ASSERT(sizeof(NLS_BindingJobHandle) == 24u, nls_job_handle_64_bit_size_changed);
    NLS_CABI_STATIC_ASSERT(sizeof(NLS_BindingJobScheduleDesc) == 64u, nls_job_schedule_desc_64_bit_size_changed);
    NLS_CABI_STATIC_ASSERT(
        sizeof(NLS_BindingForEachScheduleDesc) == 80u,
        nls_job_for_each_schedule_desc_64_bit_size_changed);
    NLS_CABI_STATIC_ASSERT(
        offsetof(NLS_BindingJobScheduleDesc, dependency) == 24u,
        nls_job_schedule_dependency_64_bit_offset_changed);
    NLS_CABI_STATIC_ASSERT(
        offsetof(NLS_BindingForEachScheduleDesc, dependency) == 40u,
        nls_job_for_each_schedule_dependency_64_bit_offset_changed);
#elif UINTPTR_MAX == UINT32_MAX
    NLS_CABI_STATIC_ASSERT(sizeof(NLS_BindingJobHandle) == 24u, nls_job_handle_32_bit_size_changed);
    NLS_CABI_STATIC_ASSERT(sizeof(NLS_BindingJobScheduleDesc) == 48u, nls_job_schedule_desc_32_bit_size_changed);
    NLS_CABI_STATIC_ASSERT(
        sizeof(NLS_BindingForEachScheduleDesc) == 64u,
        nls_job_for_each_schedule_desc_32_bit_size_changed);
    NLS_CABI_STATIC_ASSERT(
        offsetof(NLS_BindingJobScheduleDesc, dependency) == 16u,
        nls_job_schedule_dependency_32_bit_offset_changed);
    NLS_CABI_STATIC_ASSERT(
        offsetof(NLS_BindingForEachScheduleDesc, dependency) == 32u,
        nls_job_for_each_schedule_dependency_32_bit_offset_changed);
#else
    #error Unsupported pointer width for Nullus JobSystem binding ABI tests.
#endif

int NullusJobBindingCAbiCompileSmoke(void)
{
    NLS_BindingJobHandle handle;
    NLS_BindingJobScheduleDesc desc;
    NLS_BindingForEachScheduleDesc forEachDesc;
    NLS_JobStatusCode (*getWorkerCount)(uint32_t*) = &NLS_Jobs_GetWorkerCount;
    NLS_JobStatusCode (*scheduleJob)(const NLS_BindingJobScheduleDesc*, NLS_BindingJobHandle*) = &NLS_Jobs_Schedule;
    NLS_JobStatusCode (*scheduleForEach)(const NLS_BindingForEachScheduleDesc*, NLS_BindingJobHandle*) =
        &NLS_Jobs_ScheduleForEach;
    NLS_JobStatusCode (*isCompleted)(const NLS_BindingJobHandle*, uint32_t*) = &NLS_Jobs_IsCompleted;
    NLS_JobStatusCode (*complete)(NLS_BindingJobHandle*) = &NLS_Jobs_Complete;
    NLS_JobStatusCode (*clearWithoutSync)(NLS_BindingJobHandle*) = &NLS_Jobs_ClearWithoutSync;

    handle.structSize = sizeof(NLS_BindingJobHandle);
    handle.version = NLS_JOB_BINDING_VERSION;
    handle.id = 0u;
    handle.generation = 0u;
    handle.reserved = 0u;

    desc.structSize = sizeof(NLS_BindingJobScheduleDesc);
    desc.version = NLS_JOB_BINDING_VERSION;
    desc.callback = 0;
    desc.userData = 0;
    desc.dependency = handle;
    desc.priority = NLS_JOB_PRIORITY_NORMAL;
    desc.safetyPolicy = NLS_JOB_SAFETY_MAY_SYNC_WAIT;
    desc.debugName = 0;

    forEachDesc.structSize = sizeof(NLS_BindingForEachScheduleDesc);
    forEachDesc.version = NLS_JOB_BINDING_VERSION;
    forEachDesc.callback = 0;
    forEachDesc.userData = 0;
    forEachDesc.iterationCount = 0u;
    forEachDesc.batchSize = 1u;
    forEachDesc.combineCallback = 0;
    forEachDesc.dependency = handle;
    forEachDesc.priority = NLS_JOB_PRIORITY_HIGH;
    forEachDesc.safetyPolicy = NLS_JOB_SAFETY_GUARANTEED_NO_SYNC_WAIT;
    forEachDesc.debugName = 0;

    return (int)desc.priority +
        (int)desc.safetyPolicy +
        (int)forEachDesc.priority +
        (int)forEachDesc.safetyPolicy +
        (getWorkerCount != 0) +
        (scheduleJob != 0) +
        (scheduleForEach != 0) +
        (isCompleted != 0) +
        (complete != 0) +
        (clearWithoutSync != 0) +
        (int)NLS_JOB_STATUS_OK;
}
