# Contract: Binding-Ready API

## Scope

This contract defines the minimal stable C-compatible subset that future generated or script bindings can call. It does not claim that a managed C# job runtime exists in this feature.

## ABI Rules

- Public binding structs include `structSize` and `version`.
- Public binding structs do not own STL containers or C++ objects.
- Handles are opaque ids plus generations.
- All functions return explicit status codes.
- Null callbacks, stale handles, version mismatches, and struct size mismatches return deterministic errors.
- The public header is C-consumable and avoids C++-only declarations in the ABI surface.
- Completed handles are validated through a bounded retired-handle history; very old completed handles can age out and then report `NLS_JOB_STATUS_INVALID_HANDLE`.
- An all-zero `NLS_BindingJobHandle` is accepted as the default-complete handle for dependency, query, and completion calls.
- Invalid `priority` or `safetyPolicy` enum values return `NLS_JOB_STATUS_INVALID_ARGUMENT` instead of being coerced.
- Valid `priority` and `safetyPolicy` values are exposed as named C constants; callers do not need to hard-code `0` or `1`.
- Public binding descriptors use `NLS_JOB_BINDING_VERSION` for the current ABI version.
- Non-default query, complete, and clear calls return `NLS_JOB_STATUS_NOT_INITIALIZED` when the JobSystem runtime is not initialized or is already shut down.
- ABI v1 maps a completion attempt that cannot reach terminal state to `NLS_JOB_STATUS_FAILED` and leaves the binding handle set.

## Types

```c
#ifdef __cplusplus
extern "C" {
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

enum
{
    NLS_JOB_BINDING_VERSION = 1u
};

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

#ifdef __cplusplus
}
#endif
```

## Functions

```cpp
#ifdef __cplusplus
extern "C" {
#endif

NLS_JobStatusCode NLS_Jobs_GetWorkerCount(uint32_t* outWorkerCount);

NLS_JobStatusCode NLS_Jobs_Schedule(
    const NLS_BindingJobScheduleDesc* desc,
    NLS_BindingJobHandle* outHandle);

NLS_JobStatusCode NLS_Jobs_ScheduleForEach(
    const NLS_BindingForEachScheduleDesc* desc,
    NLS_BindingJobHandle* outHandle);

NLS_JobStatusCode NLS_Jobs_IsCompleted(
    const NLS_BindingJobHandle* handle,
    uint32_t* outCompleted);

NLS_JobStatusCode NLS_Jobs_Complete(NLS_BindingJobHandle* handle);
NLS_JobStatusCode NLS_Jobs_ClearWithoutSync(NLS_BindingJobHandle* handle);

#ifdef __cplusplus
}
#endif
```

## Test Obligations

- Scheduling through binding descriptors produces the same result as native scheduling.
- Invalid struct size returns `NLS_JOB_STATUS_VERSION_MISMATCH` or `NLS_JOB_STATUS_INVALID_ARGUMENT`.
- Null `outHandle` returns `NLS_JOB_STATUS_INVALID_ARGUMENT`.
- Completing a default handle succeeds and remains a no-op.
- Completing a stale non-default handle returns `NLS_JOB_STATUS_INVALID_HANDLE`.
- Querying, completing, or clearing a non-default handle while the JobSystem is stopped returns `NLS_JOB_STATUS_NOT_INITIALIZED`.
- Completing a failed job returns `NLS_JOB_STATUS_FAILED` and clears the binding handle.
- Completing a cancelled job returns `NLS_JOB_STATUS_CANCELLED` and clears the binding handle.
- A completion attempt that cannot reach a terminal status returns `NLS_JOB_STATUS_FAILED` and leaves the binding handle set.
- `JobBindings.h` must compile as a C translation unit without including C++-only Nullus headers.
- `NLS_Jobs_ScheduleForEach` with zero iterations returns `NLS_JOB_STATUS_OK`, writes a default-complete handle, and does not invoke iteration or combine callbacks.
