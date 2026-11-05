#ifndef NULLUS_PLUGIN_ABI_H
#define NULLUS_PLUGIN_ABI_H
#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#define NLS_PLUGIN_CALL __cdecl
#define NLS_PLUGIN_EXPORT __declspec(dllexport)
#define NLS_PLUGIN_OS 1u
#elif defined(__APPLE__)
#define NLS_PLUGIN_CALL
#define NLS_PLUGIN_EXPORT __attribute__((visibility("default")))
#define NLS_PLUGIN_OS 3u
#else
#define NLS_PLUGIN_CALL
#define NLS_PLUGIN_EXPORT __attribute__((visibility("default")))
#define NLS_PLUGIN_OS 2u
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
#define NLS_PLUGIN_ARCH 2u
#elif defined(_M_X64) || defined(__x86_64__)
#define NLS_PLUGIN_ARCH 1u
#else
#define NLS_PLUGIN_ARCH 0u
#endif
#define NLS_PLUGIN_ABI_MAJOR 1u
#define NLS_PLUGIN_ABI_MINOR 0u
#define NLS_PLUGIN_OK 0u
#define NLS_PLUGIN_INVALID 1u
#define NLS_PLUGIN_UNAVAILABLE 2u
#define NLS_PLUGIN_FAILED 3u
#define NLS_PLUGIN_BUFFER_TOO_SMALL 4u
#define NLS_PLUGIN_CAP_SAFE_UNLOAD UINT64_C(1)
#define NLS_PLUGIN_EVENT_BEGIN 1u
#define NLS_PLUGIN_EVENT_TICK 2u
#define NLS_PLUGIN_EVENT_END 3u
#define NLS_PLUGIN_EVENT_EDITOR_DRAW 4u
#define NLS_PLUGIN_EVENT_COMMAND 5u

#pragma pack(push, 8)
typedef uint64_t NlsPluginHandle;
typedef uint32_t NlsPluginResult;
typedef struct NlsPluginHeader
{
    uint32_t struct_size, abi_major, abi_minor, reserved;
} NlsPluginHeader;
typedef struct NlsPluginString { const char* data; uint64_t length; } NlsPluginString;
typedef struct NlsPluginCall
{
    NlsPluginHeader header;
    uint32_t event, reserved;
    double delta_seconds;
    uint64_t value;
} NlsPluginCall;
typedef NlsPluginResult (NLS_PLUGIN_CALL *NlsPluginCallback)(void* context, NlsPluginCall* call);
typedef void (NLS_PLUGIN_CALL *NlsPluginDestroy)(void* context);
typedef struct NlsPluginExtension
{
    NlsPluginHeader header;
    NlsPluginString id, kind;
    int32_t priority;
    uint32_t reserved;
    void* context;
    NlsPluginCallback invoke;
    NlsPluginDestroy destroy;
} NlsPluginExtension;

// All strings are UTF-8 and borrowed for the duration of the call. Register
// copies strings; ownership of context transfers only on successful register.
// Lifecycle/registration/invocation are host-thread-only. log/alloc/free are
// thread safe. Handles are owner/session scoped; only release is allowed after
// revocation. No exception may cross any of these function pointers.
typedef struct NlsPluginHostApi
{
    NlsPluginHeader header;
    uint64_t capabilities;
    void* host_context;
    NlsPluginHandle owner;
    uint64_t authority_id;
    NlsPluginResult (NLS_PLUGIN_CALL *register_extension)(void*, NlsPluginHandle, const NlsPluginExtension*, NlsPluginHandle*);
    NlsPluginResult (NLS_PLUGIN_CALL *unregister_extension)(void*, NlsPluginHandle, NlsPluginHandle);
    NlsPluginResult (NLS_PLUGIN_CALL *request_unload)(void*, NlsPluginHandle);
    void (NLS_PLUGIN_CALL *log)(void*, NlsPluginHandle owner, NlsPluginString);
    void* (NLS_PLUGIN_CALL *alloc)(uint64_t bytes);
    void (NLS_PLUGIN_CALL *free)(void* memory);
    NlsPluginResult (NLS_PLUGIN_CALL *read_resource)(void*, NlsPluginHandle, NlsPluginString path, void* buffer, uint64_t capacity, uint64_t* required);
} NlsPluginHostApi;
typedef struct NlsPluginApi
{
    NlsPluginHeader header;
    uint64_t capabilities;
    uint32_t os, architecture, pointer_bits, little_endian;
    NlsPluginString runtime;
    NlsPluginResult (NLS_PLUGIN_CALL *create)(const NlsPluginHostApi*, void**);
    NlsPluginResult (NLS_PLUGIN_CALL *startup)(void*);
    void (NLS_PLUGIN_CALL *request_stop)(void*);
    void (NLS_PLUGIN_CALL *abort_startup)(void*);
    void (NLS_PLUGIN_CALL *shutdown)(void*);
    NlsPluginDestroy destroy;
    // OK means all plugin-owned threads/objects that can execute independently
    // have stopped. Called on the host thread while draining, never waited on.
    NlsPluginResult (NLS_PLUGIN_CALL *is_stopped)(void*);
} NlsPluginApi;
// Query only negotiates data. It must not register, start work or allocate
// plugin state. Caller initialises out_api.header.struct_size to its capacity.
typedef NlsPluginResult (NLS_PLUGIN_CALL *NlsPluginQueryApi)(uint32_t major, uint32_t minor, NlsPluginApi* out_api);
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(NlsPluginHeader) == 16);
static_assert(offsetof(NlsPluginCall, delta_seconds) == 24);
static_assert(sizeof(NlsPluginCall) == 40);
static_assert(sizeof(void*) == 8, "Nullus native plugin ABI v1 requires a 64-bit target");
#endif
#endif
