#include "ScriptRuntime.h"
#include "ScriptScheduler.h"
#include "CoreClrScriptBackend.h"

#include <algorithm>
#include <exception>
#include <type_traits>
#include <utility>

namespace NLS::Scripting
{
namespace
{
ScriptRuntime* g_activeRuntime = nullptr;
std::unordered_map<ScriptTypeId, ScriptAsset> g_registeredScriptAssets;

ScriptValueKind ValueKind(const ScriptValue& value)
{
    return std::visit([](const auto& item)
    {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::monostate>) return ScriptValueKind::Null;
        if constexpr (std::is_same_v<Value, bool>) return ScriptValueKind::Bool;
        if constexpr (std::is_same_v<Value, int8_t>) return ScriptValueKind::Int8;
        if constexpr (std::is_same_v<Value, uint8_t>) return ScriptValueKind::UInt8;
        if constexpr (std::is_same_v<Value, int16_t>) return ScriptValueKind::Int16;
        if constexpr (std::is_same_v<Value, uint16_t>) return ScriptValueKind::UInt16;
        if constexpr (std::is_same_v<Value, int32_t>) return ScriptValueKind::Int32;
        if constexpr (std::is_same_v<Value, uint32_t>) return ScriptValueKind::UInt32;
        if constexpr (std::is_same_v<Value, int64_t>) return ScriptValueKind::Int64;
        if constexpr (std::is_same_v<Value, uint64_t>) return ScriptValueKind::UInt64;
        if constexpr (std::is_same_v<Value, float>) return ScriptValueKind::Float;
        if constexpr (std::is_same_v<Value, double>) return ScriptValueKind::Double;
        if constexpr (std::is_same_v<Value, std::string>) return ScriptValueKind::String;
        if constexpr (std::is_same_v<Value, ScriptEnumValue>) return ScriptValueKind::Enum;
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector2>) return ScriptValueKind::Vector2;
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector3>) return ScriptValueKind::Vector3;
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector4>) return ScriptValueKind::Vector4;
        if constexpr (std::is_same_v<Value, NLS::Maths::Quaternion>) return ScriptValueKind::Quaternion;
        if constexpr (std::is_same_v<Value, NLS::Maths::Color>) return ScriptValueKind::Color;
        if constexpr (std::is_same_v<Value, NativeObjectHandle>) return ScriptValueKind::ObjectReference;
        if constexpr (std::is_same_v<Value, ScriptObjectReference>) return ScriptValueKind::ObjectReference;
        return ScriptValueKind::Struct;
    }, value);
}

bool IsNumeric(ScriptValueKind kind)
{
    return kind >= ScriptValueKind::Int8 && kind <= ScriptValueKind::Double;
}

bool IsCompatibleFieldValue(const ScriptValue& value, const ScriptFieldDescriptor& oldField, const ScriptFieldDescriptor& newField)
{
    const auto actual = ValueKind(value);
    if (actual == ScriptValueKind::Null)
        return newField.type.kind == ScriptValueKind::ObjectReference || newField.type.kind == ScriptValueKind::Struct;
    if (actual != newField.type.kind && !(IsNumeric(actual) && IsNumeric(newField.type.kind)))
        return false;

    if (actual == ScriptValueKind::Enum && newField.type.id != 0)
    {
        const auto& enumValue = std::get<ScriptEnumValue>(value);
        if (enumValue.typeId != 0 && enumValue.typeId != newField.type.id)
            return false;
    }
    if (actual == ScriptValueKind::Struct && newField.type.id != 0)
    {
        const auto& structValue = std::get<ScriptStructValue>(value);
        if (structValue.typeId != 0 && structValue.typeId != newField.type.id)
            return false;
    }
    (void)oldField;
    return true;
}

const ScriptFieldDescriptor* FindReloadField(const ScriptTypeDescriptor* descriptor, const ScriptFieldDescriptor& oldField)
{
    if (!descriptor)
        return nullptr;
    for (const auto& field : descriptor->fields)
    {
        if (!field.serialized)
            continue;
        if (field.id == oldField.id || field.name == oldField.name)
            return &field;
        if (std::find(field.aliases.begin(), field.aliases.end(), oldField.name) != field.aliases.end())
            return &field;
        if (std::find(oldField.aliases.begin(), oldField.aliases.end(), field.name) != oldField.aliases.end())
            return &field;
    }
    return nullptr;
}
}

ScriptRuntime::ScriptRuntime()
    : m_scheduler(std::make_unique<ScriptScheduler>())
{
    g_activeRuntime = this;
}

ScriptRuntime::~ScriptRuntime()
{
    Shutdown();
    if (g_activeRuntime == this)
        g_activeRuntime = nullptr;
}

ScriptRuntime* GetActiveScriptRuntime()
{
    return g_activeRuntime;
}

const ScriptAsset* FindRegisteredScriptAsset(ScriptTypeId scriptTypeId)
{
    const auto found = g_registeredScriptAssets.find(scriptTypeId);
    return found == g_registeredScriptAssets.end() ? nullptr : &found->second;
}

ScriptStatus ScriptRuntime::RegisterBackend(std::unique_ptr<IScriptBackend> backend)
{
    if (!backend || !backend->GetBackendId().IsValid() || backend->GetLanguage() == ScriptLanguage::Unknown)
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Cannot register an invalid script backend.");

    const auto id = backend->GetBackendId().value;
    if (m_backends.contains(id))
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "A script backend with this ID is already registered.");
    for (const auto& [registeredId, registered] : m_backends)
    {
        (void)registeredId;
        if (registered->GetLanguage() == backend->GetLanguage())
            return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "A backend for this script language is already registered.");
    }

    if (m_initialized)
    {
        const auto language = backend->GetLanguage();
        const auto status = backend->Initialize(m_api);
        if (!status.Succeeded())
        {
            backend->Shutdown();
            return Report(status, language, {});
        }
    }

    m_backends.emplace(id, std::move(backend));
    return ScriptStatus::Ok();
}

IScriptBackend* ScriptRuntime::GetBackend(ScriptBackendId id) const
{
    const auto found = m_backends.find(id.value);
    return found == m_backends.end() ? nullptr : found->second.get();
}

IScriptBackend* ScriptRuntime::GetBackend(ScriptLanguage language) const
{
    for (const auto& [id, backend] : m_backends)
    {
        (void)id;
        if (backend->GetLanguage() == language)
            return backend.get();
    }
    return nullptr;
}

ScriptStatus ScriptRuntime::Initialize(const ScriptApiDatabase& api)
{
    if (m_initialized)
        return ScriptStatus::Ok();
    m_api = api;
    m_errors.clear();

    std::vector<IScriptBackend*> initialized;
    try
    {
        for (auto& [id, backend] : m_backends)
        {
            (void)id;
            const auto status = backend->Initialize(m_api);
            if (!status.Succeeded())
            {
                for (auto* previous : initialized)
                    previous->Shutdown();
                return Report(status, backend->GetLanguage(), {});
            }
            initialized.push_back(backend.get());
        }
    }
    catch (const std::exception& exception)
    {
        for (auto* previous : initialized)
            previous->Shutdown();
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, exception.what()), ScriptLanguage::Unknown, {});
    }
    catch (...)
    {
        for (auto* previous : initialized)
            previous->Shutdown();
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, "Unknown exception while initializing a script backend."), ScriptLanguage::Unknown, {});
    }

    m_initialized = true;
    return ScriptStatus::Ok();
}

void ScriptRuntime::Shutdown()
{
    if (m_scheduler)
        m_scheduler->Clear();
    m_scheduledFrameActive = false;
    m_scheduledCallback = ScriptCallback::Count;
    if (!m_initialized)
        return;
    for (auto& [id, backend] : m_backends)
    {
        (void)id;
        try
        {
            backend->Shutdown();
        }
        catch (...)
        {
            Report(ScriptStatus::Error(ScriptStatusCode::Exception, "A script backend threw while shutting down."), backend->GetLanguage(), {});
        }
    }
    m_initialized = false;
    m_loadedAssets.clear();
    m_instanceAssets.clear();
    m_destroyedInstances.clear();
}

IScriptBackend* ScriptRuntime::FindBackend(ScriptInstanceHandle instance) const
{
    if (!instance.IsValid())
        return nullptr;
    return GetBackend(ScriptBackendId{instance.backendId});
}

ScriptStatus ScriptRuntime::LoadScript(const ScriptAsset& asset)
{
    if (!m_initialized)
        return Report(ScriptStatus::Error(ScriptStatusCode::NotInitialized, "ScriptRuntime must be initialized before loading scripts."), asset.language, {}, &asset);
    auto* backend = GetBackend(asset.language);
    if (!backend)
        return Report(ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "No backend is registered for this script language."), asset.language, {}, &asset);
    try
    {
        const auto status = backend->LoadScript(asset);
        if (status.Succeeded())
        {
            m_loadedAssets[asset.assetId] = asset;
            if (asset.scriptType != 0)
                g_registeredScriptAssets[asset.scriptType] = asset;
        }
        return Report(status, asset.language, {}, &asset);
    }
    catch (const std::exception& exception)
    {
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, exception.what()), asset.language, {}, &asset);
    }
    catch (...)
    {
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, "Unknown exception while loading a script."), asset.language, {}, &asset);
    }
}

ScriptStatus ScriptRuntime::CreateInstance(const ScriptAsset& asset, NativeObjectHandle owner, ScriptInstanceHandle& output)
{
    output = {};
    if (!m_initialized)
        return Report(ScriptStatus::Error(ScriptStatusCode::NotInitialized, "ScriptRuntime must be initialized before creating instances."), asset.language, {}, &asset);
    // A detached component can be exercised by editor/unit-test code before it
    // is attached to a GameObject.  The zero owner is therefore allowed here;
    // a backend must treat it as a non-persistent owner and never serialize it.
    if (asset.language == ScriptLanguage::Unknown)
        return Report(ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "A script instance requires a valid language."), asset.language, {}, &asset);
    auto* backend = GetBackend(asset.language);
    if (!backend)
        return Report(ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "No backend is registered for this script language."), asset.language, {}, &asset);
    try
    {
        const auto status = backend->CreateInstance(asset, owner, output);
        if (status.Succeeded() && (!output.IsValid() || output.backendId != backend->GetBackendId().value))
        {
            output = {};
            return Report(ScriptStatus::Error(ScriptStatusCode::RuntimeError, "Backend returned an invalid script instance handle."), asset.language, {}, &asset);
        }
        if (status.Succeeded())
        {
            m_instanceAssets[output] = asset;
            m_destroyedInstances.erase(output);
        }
        return Report(status, asset.language, output, &asset);
    }
    catch (const std::exception& exception)
    {
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, exception.what()), asset.language, {}, &asset);
    }
    catch (...)
    {
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, "Unknown exception while creating a script instance."), asset.language, {}, &asset);
    }
}

ScriptStatus ScriptRuntime::DestroyInstance(ScriptInstanceHandle instance)
{
    if (!instance.IsValid())
        return ScriptStatus::Ok();
    auto* backend = FindBackend(instance);
    if (!backend)
        return Report(ScriptStatus::Error(ScriptStatusCode::InvalidHandle, "The script instance backend is no longer available."), ScriptLanguage::Unknown, instance);
    if (!m_initialized)
        return Report(ScriptStatus::Error(ScriptStatusCode::BackendClosed, "The script runtime is already shut down."), backend->GetLanguage(), instance);
    if (!m_instanceAssets.contains(instance))
    {
        if (m_destroyedInstances.contains(instance))
            return ScriptStatus::Ok();
        return Report(ScriptStatus::Error(ScriptStatusCode::InvalidHandle, "The script instance handle is not owned by this runtime."), backend->GetLanguage(), instance);
    }
    try
    {
        const auto status = backend->DestroyInstance(instance);
        if (status.Succeeded())
        {
            m_instanceAssets.erase(instance);
            m_destroyedInstances.insert(instance);
        }
        return Report(status, backend->GetLanguage(), instance);
    }
    catch (const std::exception& exception)
    {
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, exception.what()), backend->GetLanguage(), instance);
    }
    catch (...)
    {
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, "Unknown exception while destroying a script instance."), backend->GetLanguage(), instance);
    }
}

ScriptStatus ScriptRuntime::Invoke(ScriptInstanceHandle instance, ScriptCallback callback, const ScriptInvocationContext& context)
{
    if (!instance.IsValid())
        return ScriptStatus::Ok();
    auto* backend = FindBackend(instance);
    if (!backend)
        return Report(ScriptStatus::Error(ScriptStatusCode::InvalidHandle, "The script instance backend is no longer available."), ScriptLanguage::Unknown, instance);
    if (!m_initialized)
        return Report(ScriptStatus::Error(ScriptStatusCode::BackendClosed, "The script runtime is already shut down."), backend->GetLanguage(), instance);
    if (!m_instanceAssets.contains(instance))
    {
        const auto code = m_destroyedInstances.contains(instance)
            ? ScriptStatusCode::AlreadyDestroyed
            : ScriptStatusCode::InvalidHandle;
        return Report(ScriptStatus::Error(code, "The script instance handle is not owned by this runtime."), backend->GetLanguage(), instance);
    }
    try
    {
        return Report(backend->Invoke(instance, callback, context), backend->GetLanguage(), instance);
    }
    catch (const std::exception& exception)
    {
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, exception.what()), backend->GetLanguage(), instance);
    }
    catch (...)
    {
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, "Unknown exception while invoking a script callback."), backend->GetLanguage(), instance);
    }
}

ScriptStatus ScriptRuntime::InvokeBatch(ScriptCallback callback, std::span<const ScriptInstanceHandle> instances, const ScriptInvocationContext& context)
{
    if (instances.empty())
        return ScriptStatus::Ok();
    if (!m_initialized)
        return Report(ScriptStatus::Error(ScriptStatusCode::NotInitialized, "ScriptRuntime must be initialized before invoking scripts."), ScriptLanguage::Unknown, {});

    ScriptStatus firstError = ScriptStatus::Ok();
    size_t offset = 0;
    while (offset < instances.size())
    {
        auto* backend = FindBackend(instances[offset]);
        if (!backend || !m_instanceAssets.contains(instances[offset]))
        {
            const auto code = m_destroyedInstances.contains(instances[offset])
                ? ScriptStatusCode::AlreadyDestroyed
                : ScriptStatusCode::InvalidHandle;
            const auto status = Report(
                ScriptStatus::Error(code, "The script batch contains a stale or invalid instance handle."),
                backend ? backend->GetLanguage() : ScriptLanguage::Unknown,
                instances[offset]);
            if (firstError.Succeeded())
                firstError = status;
            ++offset;
            continue;
        }
        size_t end = offset + 1;
        while (end < instances.size()
            && instances[end].backendId == instances[offset].backendId
            && m_instanceAssets.contains(instances[end]))
            ++end;
        try
        {
            auto segmentContext = context;
            if (context.batchOwners.size() == instances.size())
                segmentContext.batchOwners = context.batchOwners.subspan(offset, end - offset);
            const auto status = backend->InvokeBatch(callback, instances.subspan(offset, end - offset), segmentContext);
            if (!status.Succeeded() && firstError.Succeeded())
                firstError = Report(status, backend->GetLanguage(), instances[offset]);
        }
        catch (const std::exception& exception)
        {
            if (firstError.Succeeded())
                firstError = Report(ScriptStatus::Error(ScriptStatusCode::Exception, exception.what()), backend->GetLanguage(), instances[offset]);
        }
        catch (...)
        {
            if (firstError.Succeeded())
                firstError = Report(ScriptStatus::Error(ScriptStatusCode::Exception, "Unknown exception while invoking a script batch."), backend->GetLanguage(), instances[offset]);
        }
        offset = end;
    }
    return firstError;
}

ScriptStatus ScriptRuntime::CaptureFrame(const ScriptFrameContext& frame)
{
    if (!m_initialized)
        return Report(ScriptStatus::Error(ScriptStatusCode::NotInitialized, "ScriptRuntime must be initialized before capturing a frame."), ScriptLanguage::Unknown, {});
    for (auto& [id, backend] : m_backends)
    {
        (void)id;
        try
        {
            const auto status = backend->CaptureFrame(frame);
            if (!status.Succeeded())
                return Report(status, backend->GetLanguage(), {});
        }
        catch (const std::exception& exception)
        {
            return Report(ScriptStatus::Error(ScriptStatusCode::Exception, exception.what()), backend->GetLanguage(), {});
        }
        catch (...)
        {
            return Report(ScriptStatus::Error(ScriptStatusCode::Exception, "Unknown exception while capturing a script frame."), backend->GetLanguage(), {});
        }
    }
    return ScriptStatus::Ok();
}

ScriptStatus ScriptRuntime::BeginScheduledFrame(ScriptCallback callback, const ScriptFrameContext& frame)
{
    if (!m_initialized)
        return ScriptStatus::Error(
            ScriptStatusCode::NotInitialized,
            "ScriptRuntime must be initialized before scheduling callbacks.");
    if (callback == ScriptCallback::Count)
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "A scheduled frame requires a concrete script callback.");
    if (m_scheduledFrameActive)
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "A scheduled script frame is already active.");

    m_scheduler->Clear();
    m_scheduledCallback = callback;
    m_scheduledFrame = frame;
    m_scheduledFrameActive = true;
    return ScriptStatus::Ok();
}

ScriptStatus ScriptRuntime::FlushScheduledFrame()
{
    if (!m_scheduledFrameActive)
        return ScriptStatus::Ok();

    const auto callback = m_scheduledCallback;
    const auto frame = m_scheduledFrame;
    m_scheduledCallback = ScriptCallback::Count;
    m_scheduledFrameActive = false;
    const auto status = m_scheduler->Flush(*this, callback, frame);
    m_scheduler->Clear();
    return status;
}

bool ScriptRuntime::QueueScheduledCallback(
    ScriptCallback callback,
    ScriptInstanceHandle instance,
    NativeObjectHandle owner)
{
    if (!m_scheduledFrameActive || callback != m_scheduledCallback || !instance.IsValid())
        return false;
    m_scheduler->Enqueue(instance, owner);
    return true;
}

ScriptStatus ScriptRuntime::Reload(const ScriptAsset& asset, const ScriptApiDatabase& api)
{
    // Keep the public convenience entry point on the same transaction path as
    // an explicit frame-boundary reload. This guarantees lifecycle ordering,
    // field preservation, and rollback semantics for every caller.
    return ReloadAtFrameBoundary(asset, api, {});
}

ScriptStatus ScriptRuntime::ReloadAtFrameBoundary(const ScriptAsset& asset, const ScriptApiDatabase& api, const ScriptFrameContext& frame)
{
    if (!m_initialized)
        return Report(ScriptStatus::Error(ScriptStatusCode::NotInitialized, "ScriptRuntime must be initialized before reloading scripts."), asset.language, {}, &asset);
    auto* backend = GetBackend(asset.language);
    if (!backend)
        return Report(ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "No backend is registered for this script language."), asset.language, {}, &asset);
    if (!backend->GetCapabilities().supportsHotReload)
        return Report(ScriptStatus::Error(ScriptStatusCode::Unsupported, "The registered script backend does not support hot reload."), asset.language, {}, &asset);

    const auto captureStatus = CaptureFrame(frame);
    if (!captureStatus.Succeeded())
        return captureStatus;
    if (const auto loaded = m_loadedAssets.find(asset.assetId); loaded != m_loadedAssets.end()
        && loaded->second.language != asset.language)
    {
        return Report(
            ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "A script asset cannot change language during hot reload."),
            asset.language,
            {},
            &asset);
    }
    if (api.GetSchemaHashHex() != m_api.GetSchemaHashHex())
        return Report(ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "The replacement script API schema does not match the runtime schema."), asset.language, {}, &asset);

    std::vector<ScriptInstanceHandle> affected;
    std::optional<ScriptAsset> previousAsset;
    for (const auto& [instance, instanceAsset] : m_instanceAssets)
    {
        if (instanceAsset.assetId == asset.assetId && instanceAsset.language == asset.language)
        {
            affected.push_back(instance);
            if (!previousAsset)
                previousAsset = instanceAsset;
        }
    }
    if (!previousAsset)
    {
        const auto loaded = m_loadedAssets.find(asset.assetId);
        if (loaded != m_loadedAssets.end() && loaded->second.language == asset.language)
            previousAsset = loaded->second;
    }
    if (!previousAsset)
    {
        return Report(
            ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "A script asset must be loaded before it can be hot reloaded."),
            asset.language,
            {},
            &asset);
    }
    std::sort(affected.begin(), affected.end(), [](const auto left, const auto right)
    {
        if (left.backendId != right.backendId)
            return left.backendId < right.backendId;
        if (left.generation != right.generation)
            return left.generation < right.generation;
        return left.index < right.index;
    });

    struct FieldSnapshot
    {
        ScriptInstanceHandle instance;
        ScriptFieldDescriptor field;
        ScriptValue value;
    };
    std::vector<FieldSnapshot> fieldSnapshots;
    const auto* oldDescriptor = previousAsset && previousAsset->scriptType != 0
        ? m_api.FindType(previousAsset->scriptType)
        : nullptr;

    // Compile and validate before touching live instances.  A failure leaves
    // the old callback set running and never emits synthetic lifecycle calls.
    try
    {
        const auto loadStatus = backend->LoadScript(asset);
        if (!loadStatus.Succeeded())
            return Report(loadStatus, asset.language, {}, &asset);
    }
    catch (const std::exception& exception)
    {
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, exception.what()), asset.language, {}, &asset);
    }
    catch (...)
    {
        return Report(ScriptStatus::Error(ScriptStatusCode::Exception, "Unknown exception while compiling a replacement script."), asset.language, {}, &asset);
    }

    ScriptInvocationContext context;
    context.frame = frame;
    ScriptStatus firstLifecycleError = ScriptStatus::Ok();
    for (const auto instance : affected)
    {
        const auto disableStatus = Invoke(instance, ScriptCallback::OnDisable, context);
        if (!disableStatus.Succeeded() && firstLifecycleError.Succeeded())
            firstLifecycleError = disableStatus;
    }

    // Snapshot after OnDisable so lifecycle code can intentionally update a
    // serialized field and that value survives the prototype/ALC swap.
    if (oldDescriptor)
    {
        for (const auto instance : affected)
        {
            for (const auto& field : oldDescriptor->fields)
            {
                if (!field.serialized)
                    continue;
                ScriptValue value;
                if (GetField(instance, field.id, value))
                    fieldSnapshots.push_back({instance, field, std::move(value)});
            }
        }
    }

    ScriptStatus status;
    try
    {
        status = backend->Reload(asset.assetId, api);
    }
    catch (const std::exception& exception)
    {
        status = ScriptStatus::Error(ScriptStatusCode::Exception, exception.what());
    }
    catch (...)
    {
        status = ScriptStatus::Error(ScriptStatusCode::Exception, "Unknown exception while committing a script reload.");
    }
    if (!status.Succeeded() && previousAsset)
    {
        try
        {
            // Restore the prior artifact before re-enabling instances.  The
            // backend's LoadScript contract is transactional on compilation
            // failure, so this is also safe for managed ALC swaps.
            const auto restoreStatus = backend->LoadScript(*previousAsset);
            if (!restoreStatus.Succeeded())
                status = ScriptStatus::Error(ScriptStatusCode::HotReloadRejected, "Script reload failed and the previous artifact could not be restored: " + restoreStatus.message);
        }
        catch (...)
        {
            status = ScriptStatus::Error(ScriptStatusCode::HotReloadRejected, "Script reload failed and the previous artifact could not be restored.");
        }
    }
    if (status.Succeeded())
    {
        const auto* newDescriptor = asset.scriptType != 0 ? api.FindType(asset.scriptType) : nullptr;
        for (const auto& snapshot : fieldSnapshots)
        {
            const auto* target = FindReloadField(newDescriptor, snapshot.field);
            if (!target || !IsCompatibleFieldValue(snapshot.value, snapshot.field, *target))
                continue;
            // A backend can reject a value when a generated accessor has a
            // stricter runtime conversion rule.  Such a field is left at the
            // replacement default; unrelated fields and the reload remain
            // valid and deterministic.
            try
            {
                (void)backend->SetField(snapshot.instance, target->id, snapshot.value);
            }
            catch (...)
            {
                // Field migration is best-effort after the backend commit;
                // ABI exceptions must not escape the native reload boundary.
            }
        }
        m_loadedAssets[asset.assetId] = asset;
        for (auto& [instance, instanceAsset] : m_instanceAssets)
            if (instanceAsset.assetId == asset.assetId && instanceAsset.language == asset.language)
                instanceAsset = asset;
    }
    for (const auto instance : affected)
    {
        const auto enableStatus = Invoke(instance, ScriptCallback::OnEnable, context);
        if (!enableStatus.Succeeded() && firstLifecycleError.Succeeded())
            firstLifecycleError = enableStatus;
    }
    if (status.Succeeded() && !firstLifecycleError.Succeeded())
        status = firstLifecycleError;
    return Report(status, asset.language, {}, &asset);
}

bool ScriptRuntime::GetField(ScriptInstanceHandle instance, ScriptFieldId field, ScriptValue& output) const
{
    if (!m_initialized || !instance.IsValid() || field == 0)
        return false;
    if (!m_instanceAssets.contains(instance))
        return false;
    auto* backend = FindBackend(instance);
    if (!backend)
        return false;
    try
    {
        return backend->GetField(instance, field, output);
    }
    catch (...)
    {
        return false;
    }
}

bool ScriptRuntime::SetField(ScriptInstanceHandle instance, ScriptFieldId field, const ScriptValue& value)
{
    if (!m_initialized || !instance.IsValid() || field == 0)
        return false;
    if (!m_instanceAssets.contains(instance))
        return false;
    auto* backend = FindBackend(instance);
    if (!backend)
        return false;
    try
    {
        return backend->SetField(instance, field, value);
    }
    catch (...)
    {
        return false;
    }
}

const ScriptTypeDescriptor* ScriptRuntime::FindScriptType(const ScriptAsset& asset) const
{
    if (asset.scriptType == 0)
        return nullptr;

    // Native API descriptors remain the first choice for compatibility with
    // backends that register their script types directly in the shared schema.
    if (const auto* descriptor = m_api.FindType(asset.scriptType))
        return descriptor;

    for (const auto& [id, backend] : m_backends)
    {
        (void)id;
        if (backend->GetLanguage() != asset.language)
            continue;
        if (const auto* descriptor = backend->FindScriptType(asset.scriptType))
            return descriptor;
    }
    return nullptr;
}

std::optional<bool> ScriptRuntime::IsScriptComponentAsset(const ScriptAsset& asset) const
{
    if (asset.language != ScriptLanguage::CSharp)
        return std::nullopt;
    const auto* backend = GetBackend(ScriptLanguage::CSharp);
    const auto* coreClr = dynamic_cast<const CoreClrScriptBackend*>(backend);
    return coreClr ? coreClr->IsComponentAsset(asset.sourcePath) : std::nullopt;
}

ScriptStatus ScriptRuntime::Report(
    ScriptStatus status,
    ScriptLanguage language,
    ScriptInstanceHandle instance,
    const ScriptAsset* asset)
{
    if (!status.Succeeded())
    {
        ScriptError error;
        error.language = language;
        error.backend = ScriptBackendId{instance.backendId};
        if (!error.backend.IsValid())
        {
            if (const auto* backend = GetBackend(language))
                error.backend = backend->GetBackendId();
        }
        error.instance = instance;
        if (asset)
        {
            error.scriptAsset = asset->assetId;
            error.sourcePath = asset->sourcePath;
        }
        else if (const auto found = m_instanceAssets.find(instance); found != m_instanceAssets.end())
        {
            error.scriptAsset = found->second.assetId;
            error.sourcePath = found->second.sourcePath;
        }
        error.severity = ScriptErrorSeverity::Error;
        error.message = status.message;

        IScriptBackend* backend = nullptr;
        if (error.backend.IsValid())
            backend = GetBackend(error.backend);
        if (!backend && language != ScriptLanguage::Unknown)
            backend = GetBackend(language);
        if (backend)
        {
            if (auto* provider = dynamic_cast<IScriptDiagnosticProvider*>(backend))
            {
                if (auto diagnostic = provider->ConsumeLastDiagnostic())
                {
                    if (diagnostic->language == ScriptLanguage::Unknown)
                        diagnostic->language = error.language;
                    if (!diagnostic->backend.IsValid())
                        diagnostic->backend = error.backend;
                    if (!diagnostic->instance.IsValid())
                        diagnostic->instance = error.instance;
                    if (!diagnostic->scriptAsset.IsValid())
                        diagnostic->scriptAsset = error.scriptAsset;
                    if (diagnostic->sourcePath.empty())
                        diagnostic->sourcePath = error.sourcePath;
                    if (diagnostic->message.empty())
                        diagnostic->message = error.message;
                    error = std::move(*diagnostic);
                }
            }
        }
        if (error.message.empty())
            error.message = status.message;
        m_errors.push_back(error);
        if (m_errorSink)
            m_errorSink(error);
    }
    return status;
}
}
