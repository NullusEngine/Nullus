#pragma once

#include "IScriptBackend.h"

#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Scripting
{
class ScriptScheduler;
class ScriptRuntime;

NLS_SCRIPTING_API ScriptRuntime* GetActiveScriptRuntime();
NLS_SCRIPTING_API const ScriptAsset* FindRegisteredScriptAsset(ScriptTypeId scriptTypeId);

class NLS_SCRIPTING_API ScriptRuntime final
{
public:
    using ErrorSink = std::function<void(const ScriptError&)>;

    ScriptRuntime();
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    // Before initialization this queues the backend for normal startup.  After
    // initialization it initializes and installs the backend immediately,
    // which lets an Editor Debug build attach a freshly produced CoreCLR
    // backend without recreating the ScriptRuntime context.
    ScriptStatus RegisterBackend(std::unique_ptr<IScriptBackend> backend);
    IScriptBackend* GetBackend(ScriptBackendId id) const;
    IScriptBackend* GetBackend(ScriptLanguage language) const;

    ScriptStatus Initialize(const ScriptApiDatabase& api);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }
    const std::vector<ScriptError>& GetErrors() const { return m_errors; }
    void ClearErrors() { m_errors.clear(); }

    ScriptStatus LoadScript(const ScriptAsset& asset);
    ScriptStatus CreateInstance(
        const ScriptAsset& asset,
        NativeObjectHandle owner,
        ScriptInstanceHandle& output);
    ScriptStatus DestroyInstance(ScriptInstanceHandle instance);
    ScriptStatus Invoke(
        ScriptInstanceHandle instance,
        ScriptCallback callback,
        const ScriptInvocationContext& context);
    ScriptStatus InvokeBatch(
        ScriptCallback callback,
        std::span<const ScriptInstanceHandle> instances,
        const ScriptInvocationContext& context);
    ScriptStatus CaptureFrame(const ScriptFrameContext& frame);
    // Play/editor contexts open one scheduling window per lifecycle phase.
    // ScriptComponents enqueue callbacks while the window is active; callers
    // flush at the phase boundary to preserve traversal order and isolate the
    // first backend error without stopping later backend segments.
    ScriptStatus BeginScheduledFrame(ScriptCallback callback, const ScriptFrameContext& frame);
    ScriptStatus FlushScheduledFrame();
    bool QueueScheduledCallback(
        ScriptCallback callback,
        ScriptInstanceHandle instance,
        NativeObjectHandle owner);
    ScriptStatus Reload(const ScriptAsset& asset, const ScriptApiDatabase& api);
    // Performs the reload transaction at a caller-provided frame boundary.
    // Existing instances receive only OnDisable/OnEnable around the backend
    // swap; Awake/Start/Destroy are never synthesized.
    ScriptStatus ReloadAtFrameBoundary(const ScriptAsset& asset, const ScriptApiDatabase& api, const ScriptFrameContext& frame);

    bool GetField(ScriptInstanceHandle instance, ScriptFieldId field, ScriptValue& output) const;
    bool SetField(ScriptInstanceHandle instance, ScriptFieldId field, const ScriptValue& value);
    const ScriptTypeDescriptor* FindScriptType(const ScriptAsset& asset) const;
    // Semantic C# component classification comes from the generated
    // Behaviour manifest. nullopt means the manifest is unavailable and the
    // editor may use its pre-import source fallback.
    std::optional<bool> IsScriptComponentAsset(const ScriptAsset& asset) const;

    void SetErrorSink(ErrorSink sink) { m_errorSink = std::move(sink); }
    const ScriptApiDatabase& GetApi() const { return m_api; }

private:
    ScriptStatus Report(
        ScriptStatus status,
        ScriptLanguage language,
        ScriptInstanceHandle instance,
        const ScriptAsset* asset = nullptr);
    IScriptBackend* FindBackend(ScriptInstanceHandle instance) const;

    std::unordered_map<uint16_t, std::unique_ptr<IScriptBackend>> m_backends;
    std::unordered_map<NLS::Core::Assets::AssetId, ScriptAsset> m_loadedAssets;
    std::unordered_map<ScriptInstanceHandle, ScriptAsset> m_instanceAssets;
    // Handles are scoped to this ScriptRuntime.  Keep destroyed handles long
    // enough to make repeated destruction idempotent while rejecting stale
    // handles from another runtime/context before they reach a backend.
    std::unordered_set<ScriptInstanceHandle> m_destroyedInstances;
    ScriptApiDatabase m_api;
    ErrorSink m_errorSink;
    std::vector<ScriptError> m_errors;
    std::unique_ptr<ScriptScheduler> m_scheduler;
    ScriptCallback m_scheduledCallback = ScriptCallback::Count;
    ScriptFrameContext m_scheduledFrame;
    bool m_scheduledFrameActive = false;
    bool m_initialized = false;
};
}
