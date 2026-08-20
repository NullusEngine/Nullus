#include "FakeScriptBackend.h"

namespace NLS::Scripting
{
FakeScriptBackend::FakeScriptBackend(ScriptBackendId id, ScriptLanguage language)
    : m_id(id),
      m_language(language)
{
}

ScriptStatus FakeScriptBackend::Initialize(const ScriptApiDatabase& api)
{
    if (m_initialized)
        return ScriptStatus::Ok();
    m_api = api;
    m_initialized = true;
    return ScriptStatus::Ok();
}

void FakeScriptBackend::Shutdown()
{
    m_instances.clear();
    m_scripts.clear();
    m_initialized = false;
}

ScriptStatus FakeScriptBackend::CaptureFrame(const ScriptFrameContext& frame)
{
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "Fake backend is not initialized.");
    m_frames.push_back(frame);
    return ScriptStatus::Ok();
}

ScriptStatus FakeScriptBackend::LoadScript(const ScriptAsset& asset)
{
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "Fake backend is not initialized.");
    if (asset.language != m_language || !asset.assetId.IsValid())
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Fake backend accepts valid assets for its configured language only.");
    m_scripts[asset.assetId] = asset;
    return ScriptStatus::Ok();
}

ScriptStatus FakeScriptBackend::UnloadScript(const NLS::Core::Assets::AssetId& assetId)
{
    m_scripts.erase(assetId);
    return ScriptStatus::Ok();
}

ScriptStatus FakeScriptBackend::CreateInstance(const ScriptAsset& asset, NativeObjectHandle owner, ScriptInstanceHandle& output)
{
    output = {};
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "Fake backend is not initialized.");
    if (!asset.assetId.IsValid() || !m_scripts.contains(asset.assetId))
        return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "The fake script asset has not been loaded.");
    output = {m_id.value, m_nextGeneration++, m_nextIndex++};
    m_instances.emplace(output, Instance{asset, owner, {}});
    return ScriptStatus::Ok();
}

ScriptStatus FakeScriptBackend::DestroyInstance(ScriptInstanceHandle instance)
{
    if (!instance.IsValid())
        return ScriptStatus::Ok();
    const auto found = m_instances.find(instance);
    if (found == m_instances.end())
        return m_destroyed.contains(instance)
            ? ScriptStatus::Ok()
            : ScriptStatus::Error(ScriptStatusCode::InvalidHandle, "The fake script instance does not exist.");
    m_instances.erase(found);
    m_destroyed.insert(instance);
    return ScriptStatus::Ok();
}

ScriptStatus FakeScriptBackend::Invoke(ScriptInstanceHandle instance, ScriptCallback callback, const ScriptInvocationContext& context)
{
    if (!m_instances.contains(instance))
        return ScriptStatus::Error(m_destroyed.contains(instance) ? ScriptStatusCode::AlreadyDestroyed : ScriptStatusCode::InvalidHandle, "The fake script instance does not exist.");
    m_callbacks.push_back(callback);
    if (m_observer)
        m_observer(instance, callback, context);
    return ScriptStatus::Ok();
}

ScriptStatus FakeScriptBackend::InvokeBatch(
    ScriptCallback callback,
    std::span<const ScriptInstanceHandle> instances,
    const ScriptInvocationContext& context)
{
    ++m_batchCallCount;
    m_batchInstanceCount += instances.size();
    return IScriptBackend::InvokeBatch(callback, instances, context);
}

ScriptStatus FakeScriptBackend::Reload(const NLS::Core::Assets::AssetId& assetId, const ScriptApiDatabase& api)
{
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "Fake backend is not initialized.");
    if (!m_scripts.contains(assetId))
        return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "The fake script asset has not been loaded.");
    if (api.GetSchemaHashHex() != m_api.GetSchemaHashHex())
        return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "The fake backend schema does not match.");
    m_api = api;
    const auto& replacement = m_scripts.at(assetId);
    for (auto& [instance, state] : m_instances)
    {
        (void)instance;
        if (state.asset.assetId == assetId)
            state.asset = replacement;
    }
    return ScriptStatus::Ok();
}

bool FakeScriptBackend::GetField(ScriptInstanceHandle instance, ScriptFieldId field, ScriptValue& output)
{
    const auto found = m_instances.find(instance);
    if (found == m_instances.end())
        return false;
    const auto value = found->second.fields.find(field);
    if (value == found->second.fields.end())
        return false;
    output = value->second;
    return true;
}

bool FakeScriptBackend::SetField(ScriptInstanceHandle instance, ScriptFieldId field, const ScriptValue& value)
{
    const auto found = m_instances.find(instance);
    if (found == m_instances.end() || field == 0)
        return false;
    found->second.fields[field] = value;
    return true;
}

void FakeScriptBackend::ClearTrace()
{
    m_callbacks.clear();
    m_frames.clear();
    m_destroyed.clear();
    m_batchCallCount = 0;
    m_batchInstanceCount = 0;
}
}
