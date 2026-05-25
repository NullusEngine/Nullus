#pragma once

#include <stdint.h>

#ifndef NLS_BASE_API
    #if defined(NLS_SHARED_LIB)
        #if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
            #if defined(NLS_BASE_EXPORT)
                #define NLS_BASE_API __declspec(dllexport)
            #else
                #define NLS_BASE_API __declspec(dllimport)
            #endif
        #elif defined(__GNUC__) || defined(__clang__)
            #define NLS_BASE_API __attribute__((visibility("default")))
        #else
            #define NLS_BASE_API
        #endif
    #else
        #define NLS_BASE_API
    #endif
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum NLS_JobStatusCode
    {
        NLS_JOB_STATUS_OK = 0,
        NLS_JOB_STATUS_NOT_INITIALIZED = 1,
        NLS_JOB_STATUS_INVALID_ARGUMENT = 2,
        NLS_JOB_STATUS_INVALID_HANDLE = 3,
        NLS_JOB_STATUS_VERSION_MISMATCH = 4,
        NLS_JOB_STATUS_CANCELLED = 5,
        NLS_JOB_STATUS_FAILED = 6
    } NLS_JobStatusCode;

    enum
    {
        NLS_JOB_BINDING_VERSION = 1u
    };

    typedef enum NLS_JobBindingPriority
    {
        NLS_JOB_PRIORITY_NORMAL = 0,
        NLS_JOB_PRIORITY_HIGH = 1
    } NLS_JobBindingPriority;

    typedef enum NLS_JobBindingSafetyPolicy
    {
        NLS_JOB_SAFETY_MAY_SYNC_WAIT = 0,
        NLS_JOB_SAFETY_GUARANTEED_NO_SYNC_WAIT = 1
    } NLS_JobBindingSafetyPolicy;

    typedef struct NLS_BindingJobHandle
    {
        uint32_t structSize;
        uint32_t version;
        uint64_t id;
        uint32_t generation;
        uint32_t reserved;
    } NLS_BindingJobHandle;

    typedef void (*NLS_BindingJobCallback)(void* userData);
    typedef void (*NLS_BindingForEachCallback)(void* userData, uint32_t index);

    typedef struct NLS_BindingJobScheduleDesc
    {
        uint32_t structSize;
        uint32_t version;
        NLS_BindingJobCallback callback;
        void* userData;
        NLS_BindingJobHandle dependency;
        uint32_t priority;
        uint32_t safetyPolicy;
        const char* debugName;
    } NLS_BindingJobScheduleDesc;

    typedef struct NLS_BindingForEachScheduleDesc
    {
        uint32_t structSize;
        uint32_t version;
        NLS_BindingForEachCallback callback;
        void* userData;
        uint32_t iterationCount;
        uint32_t batchSize;
        NLS_BindingJobCallback combineCallback;
        NLS_BindingJobHandle dependency;
        uint32_t priority;
        uint32_t safetyPolicy;
        const char* debugName;
    } NLS_BindingForEachScheduleDesc;

    NLS_BASE_API NLS_JobStatusCode NLS_Jobs_GetWorkerCount(uint32_t* outWorkerCount);
    NLS_BASE_API NLS_JobStatusCode NLS_Jobs_Schedule(
        const NLS_BindingJobScheduleDesc* desc,
        NLS_BindingJobHandle* outHandle);
    NLS_BASE_API NLS_JobStatusCode NLS_Jobs_ScheduleForEach(
        const NLS_BindingForEachScheduleDesc* desc,
        NLS_BindingJobHandle* outHandle);
    NLS_BASE_API NLS_JobStatusCode NLS_Jobs_IsCompleted(
        const NLS_BindingJobHandle* handle,
        uint32_t* outCompleted);
    NLS_BASE_API NLS_JobStatusCode NLS_Jobs_Complete(NLS_BindingJobHandle* handle);
    NLS_BASE_API NLS_JobStatusCode NLS_Jobs_ClearWithoutSync(NLS_BindingJobHandle* handle);

#ifdef __cplusplus
}
#endif
