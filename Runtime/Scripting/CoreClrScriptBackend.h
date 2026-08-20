#pragma once

#include "IScriptBackend.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NLS::Engine::SceneSystem { class Scene; }

namespace NLS::Scripting
{

// The C# native callbacks are process-wide function pointers.  The current
// scene is refreshed at each script callback so static lookups such as
// GameObject.Find remain scoped to the active runtime scene.
NLS_SCRIPTING_API void SetNativeScriptScene(NLS::Engine::SceneSystem::Scene* scene);

struct NLS_SCRIPTING_API ScriptAbiHeader
{
    uint32_t size = sizeof(ScriptAbiHeader);
    uint32_t abiVersion = 2;
    uint64_t schemaHash = 0;
};

struct NLS_SCRIPTING_API ScriptAbiResult
{
    ScriptStatusCode code = ScriptStatusCode::Ok;
    const char* message = nullptr;
};

// Fixed-layout field value used only at the Native/Managed ABI boundary.  It
// deliberately contains no STL object, variant, or ownership-bearing string.
// utf8Data/bytes are borrowed for the duration of the call.
struct NLS_SCRIPTING_API ScriptAbiValue
{
    uint32_t kind = 0;
    uint32_t reserved = 0;
    uint64_t typeId = 0;
    int64_t signedValue = 0;
    uint64_t unsignedValue = 0;
    double floatingValue = 0.0;
    uint64_t objectValue = 0;
    const char* utf8Data = nullptr;
    uint32_t utf8Size = 0;
    const uint8_t* bytes = nullptr;
    uint32_t byteSize = 0;
};

struct NativeApiTable;

using ManagedInitializeFn = ScriptAbiResult (*)(ScriptAbiHeader*, const char* schemaHashHex, const NativeApiTable* nativeApi);
using ManagedShutdownFn = ScriptAbiResult (*)();
using ManagedLoadScriptFn = ScriptAbiResult (*)(uint64_t assetId, const char* assetPath, const char* source, uint64_t* contentHash);
using ManagedCreateInstanceFn = ScriptAbiResult (*)(uint64_t assetId, uint64_t owner, uint64_t* managedToken);
using ManagedDestroyInstanceFn = ScriptAbiResult (*)(uint64_t managedToken);
using ManagedInvokeFn = ScriptAbiResult (*)(uint64_t managedToken, uint16_t callback, const ScriptFrameContext* frame, uint64_t owner);
using ManagedInvokeBatchFn = ScriptAbiResult (*)(
    uint16_t callback,
    const uint64_t* managedTokens,
    const uint64_t* owners,
    uint32_t count,
    const ScriptFrameContext* frame);
using ManagedReloadFn = ScriptAbiResult (*)(uint64_t assetId, const char* source, const char* schemaHashHex);
// assetId == 0 requests a project-wide GameScripts assembly swap.
using ManagedReloadAssemblyFn = ScriptAbiResult (*)(uint64_t assetId, const char* assemblyPath, const char* schemaHashHex);
using ManagedGetFieldFn = ScriptAbiResult (*)(uint64_t managedToken, uint64_t field, ScriptAbiValue* output);
using ManagedSetFieldFn = ScriptAbiResult (*)(uint64_t managedToken, uint64_t field, const ScriptAbiValue* value);

// All string pointers are borrowed from Managed until the next ABI call.
// Native copies them immediately into ScriptError before reporting it.
struct NLS_SCRIPTING_API ScriptAbiDiagnostic
{
    uint32_t size = sizeof(ScriptAbiDiagnostic);
    uint32_t severity = static_cast<uint32_t>(ScriptErrorSeverity::Error);
    int32_t line = 0;
    int32_t column = 0;
    const char* sourcePath = nullptr;
    const char* message = nullptr;
    const char* stackTrace = nullptr;
};

using ManagedGetLastDiagnosticFn = ScriptAbiDiagnostic* (*)();
// The returned UTF-8 buffer is owned by Managed and remains valid until the
// next managed ABI call. Native copies it before parsing or exposing it.
using ManagedGetBehaviourManifestFn = ScriptAbiResult (*)(const char** data, uint32_t* size);

struct NLS_SCRIPTING_API ManagedApiTable
{
    ScriptAbiHeader header;
    ManagedInitializeFn initialize = nullptr;
    ManagedShutdownFn shutdown = nullptr;
    ManagedLoadScriptFn loadScript = nullptr;
    ManagedCreateInstanceFn createInstance = nullptr;
    ManagedDestroyInstanceFn destroyInstance = nullptr;
    ManagedInvokeFn invoke = nullptr;
    ManagedReloadFn reload = nullptr;
    ManagedReloadAssemblyFn reloadAssembly = nullptr;
    ManagedGetFieldFn getField = nullptr;
    ManagedSetFieldFn setField = nullptr;
    ManagedInvokeBatchFn invokeBatch = nullptr;
    ManagedGetLastDiagnosticFn getLastDiagnostic = nullptr;
    ManagedGetBehaviourManifestFn getBehaviourManifest = nullptr;

    ManagedApiTable()
    {
        header.size = sizeof(ManagedApiTable);
    }
};

struct NLS_SCRIPTING_API ManagedBehaviourManifestEntry
{
    struct Field
    {
        ScriptFieldId id = 0;
        std::string name;
        std::string typeName;
        std::string kind;
        std::vector<std::string> aliases;
        bool serialized = true;
    };

    std::string assetPath;
    std::string fileName;
    std::string typeName;
    std::string simpleName;
    ScriptTypeId scriptTypeId = 0;
    bool isPublic = false;
    bool isConcrete = false;
    bool hasNativeObjectHandleConstructor = false;
    bool isComponent = false;
    uint32_t callbackMask = 0;
    std::vector<Field> fields;
};

struct NLS_SCRIPTING_API NativeApiTable
{
    ScriptAbiHeader header;
    void (*log)(ScriptErrorSeverity severity, const char* message) = nullptr;

    // Portable Native calls exposed to generated managed wrappers.  Every
    // function uses fixed-width values and returns a structured result so no
    // C++ exception or owning C++ type crosses the ABI.
    uint8_t (*isAlive)(uint64_t handle) = nullptr;
    ScriptAbiResult (*getObject)(uint64_t owner, uint64_t memberId, uint64_t* output) = nullptr;
    ScriptAbiResult (*getVector3)(uint64_t owner, uint64_t memberId, float* x, float* y, float* z) = nullptr;
    ScriptAbiResult (*setVector3)(uint64_t owner, uint64_t memberId, float x, float y, float z) = nullptr;
    ScriptAbiResult (*setBool)(uint64_t owner, uint64_t memberId, uint8_t value) = nullptr;
    ScriptAbiResult (*getQuaternion)(uint64_t owner, uint64_t memberId, float* x, float* y, float* z, float* w) = nullptr;
    ScriptAbiResult (*setQuaternion)(uint64_t owner, uint64_t memberId, float x, float y, float z, float w) = nullptr;
    ScriptAbiResult (*createPrimitive)(uint64_t owner, const char* typeName, uint64_t* output) = nullptr;
    ScriptAbiResult (*getString)(uint64_t owner, uint64_t memberId, const char** output, uint32_t* size) = nullptr;
    ScriptAbiResult (*setString)(uint64_t owner, uint64_t memberId, const char* value) = nullptr;
    ScriptAbiResult (*getBool)(uint64_t owner, uint64_t memberId, uint8_t* output) = nullptr;
    ScriptAbiResult (*getComponent)(uint64_t owner, uint64_t typeId, uint64_t* output) = nullptr;
    ScriptAbiResult (*getInt32)(uint64_t owner, uint64_t memberId, int32_t* output) = nullptr;
    ScriptAbiResult (*setInt32)(uint64_t owner, uint64_t memberId, int32_t value) = nullptr;
    // These two slots are part of the v2 layout even when no generated
    // binding currently exposes a scalar float property.
    ScriptAbiResult (*getFloat)(uint64_t owner, uint64_t memberId, float* output) = nullptr;
    ScriptAbiResult (*setFloat)(uint64_t owner, uint64_t memberId, float value) = nullptr;
    ScriptAbiResult (*destroy)(uint64_t owner, float delay) = nullptr;
    ScriptAbiResult (*instantiate)(uint64_t owner, uint64_t* output) = nullptr;
    ScriptAbiResult (*find)(const char* name, uint64_t* output) = nullptr;
    ScriptAbiResult (*findWithTag)(const char* tag, uint64_t* output) = nullptr;
    ScriptAbiResult (*addComponent)(uint64_t owner, uint64_t typeId, uint64_t* output) = nullptr;
    ScriptAbiResult (*setObject)(uint64_t owner, uint64_t memberId, uint64_t value) = nullptr;
    ScriptAbiResult (*getKey)(int32_t key, uint8_t* output) = nullptr;
    ScriptAbiResult (*getKeyDown)(int32_t key, uint8_t* output) = nullptr;
    ScriptAbiResult (*getKeyUp)(int32_t key, uint8_t* output) = nullptr;
    ScriptAbiResult (*getMouseButton)(int32_t button, uint8_t* output) = nullptr;
    ScriptAbiResult (*getMouseButtonDown)(int32_t button, uint8_t* output) = nullptr;
    ScriptAbiResult (*getMouseButtonUp)(int32_t button, uint8_t* output) = nullptr;
    ScriptAbiResult (*getMousePosition)(float* x, float* y) = nullptr;
    ScriptAbiResult (*getMouseScrollDelta)(float* x, float* y) = nullptr;
    ScriptAbiResult (*getTimeScale)(float* output) = nullptr;
    ScriptAbiResult (*setTimeScale)(float value) = nullptr;
    ScriptAbiResult (*createGameObject)(const char* name, const char* tag, uint64_t* output) = nullptr;
    ScriptAbiResult (*getActiveScene)(const char** path, uint32_t* size, uint8_t* loaded) = nullptr;
    ScriptAbiResult (*loadScene)(const char* path) = nullptr;

    NativeApiTable()
    {
        header.size = sizeof(NativeApiTable);
    }
};

class NLS_SCRIPTING_API CoreClrHost final
{
public:
    ~CoreClrHost();

    static std::filesystem::path LocateHostFxr(const std::filesystem::path& dotnetRoot = {});
    void SetDotnetRoot(std::filesystem::path dotnetRoot)
    {
        m_dotnetRoot = std::move(dotnetRoot);
    }
    ScriptStatus Load(const std::filesystem::path& runtimeConfig, const std::filesystem::path& assembly);
    // Resolve a function exported from a managed assembly through hostfxr's
    // load_assembly_and_get_function_pointer delegate.  The resolved pointer
    // remains valid until Unload and must use an UnmanagedCallersOnly ABI.
    ScriptStatus GetUnmanagedFunctionPointer(
        const std::filesystem::path& assembly,
        std::string_view typeName,
        std::string_view methodName,
        void*& output) const;
    void Unload();
    bool IsLoaded() const { return m_loaded; }

private:
#if defined(_WIN32)
#define NLS_CORECLR_DELEGATE_CALLTYPE __stdcall
    using HostFxrCloseFn = int32_t (NLS_CORECLR_DELEGATE_CALLTYPE *)(void*);
    using HostFxrGetRuntimeDelegateFn = int32_t (NLS_CORECLR_DELEGATE_CALLTYPE *)(void*, int32_t, void**);
    using LoadAssemblyAndGetFunctionPointerFn = int (NLS_CORECLR_DELEGATE_CALLTYPE *)(
        const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, void*, void**);
#else
#define NLS_CORECLR_DELEGATE_CALLTYPE
    using HostFxrCloseFn = int32_t (*)(void*);
    using HostFxrGetRuntimeDelegateFn = int32_t (*)(void*, int32_t, void**);
    using LoadAssemblyAndGetFunctionPointerFn = int (*)(
        const char*, const char*, const char*, const char*, void*, void**);
#endif
#if defined(_WIN32)
    using HostFxrErrorWriterFn = void (NLS_CORECLR_DELEGATE_CALLTYPE *)(const wchar_t*);
#else
    using HostFxrErrorWriterFn = void (*)(const char*);
#endif
    using HostFxrSetErrorWriterFn = HostFxrErrorWriterFn (NLS_CORECLR_DELEGATE_CALLTYPE *)(HostFxrErrorWriterFn);

    std::filesystem::path m_runtimeConfig;
    std::filesystem::path m_assembly;
    std::filesystem::path m_dotnetRoot;
    void* m_library = nullptr;
    void* m_context = nullptr;
    HostFxrCloseFn m_close = nullptr;
    HostFxrGetRuntimeDelegateFn m_getRuntimeDelegate = nullptr;
    HostFxrSetErrorWriterFn m_setErrorWriter = nullptr;
    LoadAssemblyAndGetFunctionPointerFn m_loadAssemblyAndGetFunctionPointer = nullptr;
    bool m_loaded = false;
};

class NLS_SCRIPTING_API CoreClrScriptBackend final : public IScriptBackend, public IScriptDiagnosticProvider
{
public:
    explicit CoreClrScriptBackend(ScriptBackendId id = {1});

    ScriptBackendId GetBackendId() const override { return m_id; }
    ScriptLanguage GetLanguage() const override { return ScriptLanguage::CSharp; }
    ScriptBackendCapabilities GetCapabilities() const override { return {m_managedApi.invokeBatch != nullptr, m_managedApi.reloadAssembly != nullptr, true, true}; }
    ScriptStatus Initialize(const ScriptApiDatabase& api) override;
    void Shutdown() override;
    ScriptStatus CaptureFrame(const ScriptFrameContext& frame) override;
    ScriptStatus LoadScript(const ScriptAsset& asset) override;
    ScriptStatus UnloadScript(const NLS::Core::Assets::AssetId& assetId) override;
    ScriptStatus CreateInstance(const ScriptAsset& asset, NativeObjectHandle owner, ScriptInstanceHandle& output) override;
    ScriptStatus DestroyInstance(ScriptInstanceHandle instance) override;
    ScriptStatus Invoke(ScriptInstanceHandle instance, ScriptCallback callback, const ScriptInvocationContext& context) override;
    ScriptStatus InvokeBatch(
        ScriptCallback callback,
        std::span<const ScriptInstanceHandle> instances,
        const ScriptInvocationContext& context) override;
    ScriptStatus Reload(const NLS::Core::Assets::AssetId& assetId, const ScriptApiDatabase& api) override;
    // Swap the project GameScripts assembly at a frame boundary.  Asset id 0
    // is reserved for this project-wide operation in the optional ABI entry.
    // Existing managed instances are recreated by the collectible ALC and
    // receive only OnDisable/OnEnable around the swap.
    ScriptStatus ReloadProjectAssembly(const std::filesystem::path& assembly, const ScriptApiDatabase& api);
    // Returns nullopt when no semantic manifest is available yet (for
    // example before the first project compile or with an older Managed ABI).
    std::optional<bool> IsComponentAsset(std::string_view assetPath) const;
    const ScriptTypeDescriptor* FindScriptType(ScriptTypeId id) const override;
    const std::vector<ManagedBehaviourManifestEntry>& GetBehaviourManifest() const
    {
        return m_behaviourManifest;
    }
    bool GetField(ScriptInstanceHandle instance, ScriptFieldId field, ScriptValue& output) override;
    bool SetField(ScriptInstanceHandle instance, ScriptFieldId field, const ScriptValue& value) override;
    std::optional<ScriptError> ConsumeLastDiagnostic() override;

    ScriptStatus SetManagedApi(const ManagedApiTable& table);
    ScriptStatus BindManagedApi(
        std::string_view typeName = "Nullus.GameScripts.GameScriptsExports, GameScripts",
        std::string_view methodName = "GetApiTable");
    void SetHostArtifacts(std::filesystem::path runtimeConfig, std::filesystem::path assembly)
    {
        m_runtimeConfig = std::move(runtimeConfig);
        m_assembly = std::move(assembly);
    }
    // Pin hostfxr lookup to the project's bundled SDK/runtime.  This keeps
    // script startup independent from DOTNET_ROOT and machine-wide installs.
    void SetDotnetRoot(std::filesystem::path dotnetRoot)
    {
        m_host.SetDotnetRoot(std::move(dotnetRoot));
    }
    void SetNativeApi(NativeApiTable table)
    {
        m_nativeApi = table;
        InstallDefaultNativeApi();
    }
    const CoreClrHost& GetHost() const { return m_host; }

    // Diagnostics used by scheduler benchmarks. These counters measure calls
    // across the Native/Managed ABI, not managed callbacks inside a batch.
    size_t GetInvokeCallCount() const { return m_invokeCallCount; }
    size_t GetBatchCallCount() const { return m_batchCallCount; }
    size_t GetBatchInstanceCount() const { return m_batchInstanceCount; }
    void ClearInvocationTrace()
    {
        m_invokeCallCount = 0;
        m_batchCallCount = 0;
        m_batchInstanceCount = 0;
    }

private:
    struct LoadedAsset
    {
        ScriptAsset asset;
        uint64_t contentHash = 0;
    };
    struct Instance
    {
        uint64_t managedToken = 0;
        NativeObjectHandle owner;
        ScriptAsset asset;
    };

    ScriptStatus FromAbiResult(ScriptAbiResult result, ScriptLanguage language, ScriptInstanceHandle instance = {});
    ScriptStatus ValidateTable(bool validateSchema) const;
    ScriptStatus RefreshBehaviourManifest();
    void InstallDefaultNativeApi();

    ScriptBackendId m_id;
    std::filesystem::path m_runtimeConfig;
    std::filesystem::path m_assembly;
    ScriptApiDatabase m_api;
    CoreClrHost m_host;
    ManagedApiTable m_managedApi;
    NativeApiTable m_nativeApi;
    std::unordered_map<NLS::Core::Assets::AssetId, LoadedAsset> m_assets;
    std::unordered_map<ScriptInstanceHandle, Instance> m_instances;
    std::unordered_set<ScriptInstanceHandle> m_destroyed;
    std::vector<ScriptFrameContext> m_frames;
    std::vector<uint64_t> m_batchTokens;
    std::vector<uint64_t> m_batchOwners;
    size_t m_invokeCallCount = 0;
    size_t m_batchCallCount = 0;
    size_t m_batchInstanceCount = 0;
    std::optional<ScriptError> m_lastDiagnostic;
    std::vector<ManagedBehaviourManifestEntry> m_behaviourManifest;
    std::unordered_map<ScriptTypeId, ScriptTypeDescriptor> m_scriptTypes;
    bool m_behaviourManifestLoaded = false;
    uint32_t m_nextIndex = 1;
    uint16_t m_nextGeneration = 1;
    bool m_initialized = false;
};
}
