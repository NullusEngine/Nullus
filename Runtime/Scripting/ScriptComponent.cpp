#include "ScriptComponent.h"
#include "CoreClrScriptBackend.h"

#include "Gen/MetaGenerated.h"
#include <GameObject.h>
#include <Serialize/ObjectGraphDocument.h>
#include <Debug/Logger.h>

#include <charconv>
#include <optional>
#include <stdexcept>

namespace NLS::Scripting
{
namespace
{
const NLS::Engine::Serialize::PropertyValue* FindProperty(
    const std::vector<NLS::Engine::Serialize::PropertyRecord>& properties,
    std::string_view name)
{
    for (const auto& property : properties)
        if (property.name == name)
            return &property.value;
    return nullptr;
}

bool HasProperty(
    const std::vector<NLS::Engine::Serialize::PropertyRecord>& properties,
    std::string_view name)
{
    return FindProperty(properties, name) != nullptr;
}

std::optional<std::string> ReadString(
    const std::vector<NLS::Engine::Serialize::PropertyRecord>& properties,
    std::string_view name)
{
    const auto* value = FindProperty(properties, name);
    if (!value || value->GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::String)
        return std::nullopt;
    return value->GetString();
}

std::optional<int64_t> ReadInteger(
    const std::vector<NLS::Engine::Serialize::PropertyRecord>& properties,
    std::string_view name)
{
    const auto* value = FindProperty(properties, name);
    if (!value || value->GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Integer)
        return std::nullopt;
    return value->GetInteger();
}
}

ScriptComponent::ScriptComponent()
{
    // Pull the Scripting generated module into static-library consumers and
    // make its reflected type available to ObjectGraph type-name loading.
    NLS_META_GENERATED_LINK_FUNCTION();
}

ScriptComponent::~ScriptComponent()
{
    // Components can be removed directly from a GameObject without going
    // through DestroyFromOwner.  Release the backend instance here as a
    // final ownership boundary; OnDestroy remains idempotent for the normal
    // engine destruction path.
    OnDestroy();
}

void ScriptComponent::SetRuntime(ScriptRuntime* runtime)
{
    m_runtime = runtime;
    if (!m_runtime || m_pendingSerializedFields.empty())
        return;

    SerializedScriptFields fields;
    OrphanScriptFields orphans;
    const auto status = ScriptFieldSerialization::Deserialize(
        m_pendingSerializedFields,
        GetScriptTypeDescriptor(),
        fields,
        orphans);
    if (status.Succeeded())
    {
        m_fields = std::move(fields);
        m_orphanFields = std::move(orphans);
        m_pendingSerializedFields.clear();
    }
}

void ScriptComponent::SetScriptAsset(ScriptAsset asset)
{
    if (m_instance.IsValid() && m_runtime)
        m_runtime->DestroyInstance(m_instance);
    m_instance = {};
    m_asset = std::move(asset);
    m_pendingSerializedFields.clear();
    m_destroyed = false;
    m_awakeCalled = false;
}

void ScriptComponent::ResolveScriptAsset(ScriptAsset asset)
{
    if (!asset.assetId.IsValid())
        return;
    if (m_asset.assetId.IsValid() && m_asset.assetId != asset.assetId)
        return;
    if (m_asset.language != ScriptLanguage::Unknown &&
        asset.language != ScriptLanguage::Unknown &&
        m_asset.language != asset.language)
    {
        return;
    }
    if (m_instance.IsValid() || m_awakeCalled)
        return;

    if (!m_asset.assetId.IsValid())
        m_asset.assetId = asset.assetId;
    if (m_asset.language == ScriptLanguage::Unknown)
        m_asset.language = asset.language;
    if (!asset.sourcePath.empty())
        m_asset.sourcePath = std::move(asset.sourcePath);
    if (!asset.sourceText.empty())
        m_asset.sourceText = std::move(asset.sourceText);
    if (asset.scriptType != 0)
        m_asset.scriptType = asset.scriptType;
    if (asset.contentHash != 0)
        m_asset.contentHash = asset.contentHash;
    m_asset.isComponent = asset.isComponent;
}

bool ScriptComponent::SetSerializedField(ScriptFieldId field, ScriptValue value)
{
    if (field == 0)
        return false;

    const auto found = m_fields.find(field);
    const auto previous = found == m_fields.end() ? std::optional<ScriptValue>{} : std::optional<ScriptValue>{found->second};
    m_fields[field] = value;
    m_pendingSerializedFields.clear();
    if (!m_runtime || !m_instance.IsValid() || m_runtime->SetField(m_instance, field, value))
        return true;

    // Keep the serialized state and the live instance consistent when a
    // backend rejects a field write.
    if (previous)
        m_fields[field] = *previous;
    else
        m_fields.erase(field);
    return false;
}

bool ScriptComponent::GetRuntimeSerializedField(ScriptFieldId field, ScriptValue& output) const
{
    if (!m_runtime || !m_instance.IsValid() || field == 0)
        return false;
    return m_runtime->GetField(m_instance, field, output);
}

void ScriptComponent::RestoreSerializedState(ScriptAsset asset, std::string serializedFields)
{
    if (m_instance.IsValid() && m_runtime)
        m_runtime->DestroyInstance(m_instance);
    m_instance = {};
    m_asset = std::move(asset);
    m_pendingSerializedFields = std::move(serializedFields);
    m_fields.clear();
    m_orphanFields.clear();
    m_destroyed = false;
    m_awakeCalled = false;

    if (!m_pendingSerializedFields.empty())
    {
        SerializedScriptFields fields;
        OrphanScriptFields orphans;
        const auto status = ScriptFieldSerialization::Deserialize(
            m_pendingSerializedFields,
            GetScriptTypeDescriptor(),
            fields,
            orphans);
        if (!status.Succeeded())
            throw std::invalid_argument(status.message);
        m_fields = std::move(fields);
        m_orphanFields = std::move(orphans);
    }
}

const ScriptTypeDescriptor* ScriptComponent::GetScriptTypeDescriptor() const
{
    return m_runtime ? m_runtime->FindScriptType(m_asset) : nullptr;
}

void ScriptComponent::OnCreate()
{
    m_destroyed = false;
    m_awakeCalled = false;
}

void ScriptComponent::OnAwake()
{
    if (m_destroyed || m_awakeCalled || m_instance.IsValid() || !m_runtime || m_asset.language == ScriptLanguage::Unknown)
        return;
    const auto loadStatus = m_runtime->LoadScript(m_asset);
    if (!loadStatus.Succeeded())
    {
        NLS_LOG_ERROR(
            "ScriptComponent failed to load '" +
            (m_asset.sourcePath.empty() ? m_asset.assetId.ToString() : m_asset.sourcePath) +
            "' during Awake: " + loadStatus.message);
        return;
    }
    // The managed Behaviour is bound to its own Component instance.  Its
    // gameObject property resolves the host through the native component
    // owner relation, so multiple scripts on one object keep distinct handles.
    const auto owner = NativeObjectHandle::FromInstanceId(GetInstanceID());
    const auto createStatus = m_runtime->CreateInstance(m_asset, owner, m_instance);
    if (!createStatus.Succeeded())
    {
        NLS_LOG_ERROR(
            "ScriptComponent failed to create '" +
            (m_asset.sourcePath.empty() ? m_asset.assetId.ToString() : m_asset.sourcePath) +
            "' during Awake: " + createStatus.message);
        return;
    }
    m_awakeCalled = true;
    for (const auto& [field, value] : m_fields)
        m_runtime->SetField(m_instance, field, value);
    Invoke(ScriptCallback::Awake, 0.0f);
}

void ScriptComponent::OnStart() { Invoke(ScriptCallback::Start, 0.0f); }
void ScriptComponent::OnEnable() { Invoke(ScriptCallback::OnEnable, 0.0f); }
void ScriptComponent::OnDisable() { Invoke(ScriptCallback::OnDisable, 0.0f); }
void ScriptComponent::OnUpdate(float deltaTime) { Invoke(ScriptCallback::Update, deltaTime); }
void ScriptComponent::OnFixedUpdate(float deltaTime) { Invoke(ScriptCallback::FixedUpdate, deltaTime); }
void ScriptComponent::OnLateUpdate(float deltaTime) { Invoke(ScriptCallback::LateUpdate, deltaTime); }

void ScriptComponent::OnDestroy()
{
    if (!m_destroyed && m_runtime && m_instance.IsValid())
    {
        Invoke(ScriptCallback::OnDestroy, 0.0f);
        m_runtime->DestroyInstance(m_instance);
    }
    m_instance = {};
    m_destroyed = true;
}

void ScriptComponent::Invoke(ScriptCallback callback, float deltaTime)
{
    if (m_destroyed || !m_runtime || !m_instance.IsValid())
        return;
    const auto owner = NativeObjectHandle::FromInstanceId(GetInstanceID());
    if (auto* host = gameobject())
        SetNativeScriptScene(host->GetScene());
    if (m_runtime->QueueScheduledCallback(callback, m_instance, owner))
        return;

    ScriptInvocationContext context;
    context.frame.deltaTime = deltaTime;
    context.frame.unscaledDeltaTime = deltaTime;
    context.owner = owner;
    m_runtime->Invoke(m_instance, callback, context);
}

bool ScriptComponent::SerializeObjectGraphProperties(
    std::vector<NLS::Engine::Serialize::PropertyRecord>& properties) const
{
    std::string serializedFields;
    const auto status = ScriptFieldSerialization::Serialize(m_fields, m_orphanFields, serializedFields);
    if (!status.Succeeded())
        throw std::invalid_argument(status.message);

    properties.push_back({"assetId", NLS::Engine::Serialize::PropertyValue::String(m_asset.assetId.ToString())});
    properties.push_back({"language", NLS::Engine::Serialize::PropertyValue::Integer(
        static_cast<int64_t>(m_asset.language))});
    properties.push_back({"scriptTypeId", NLS::Engine::Serialize::PropertyValue::String(
        std::to_string(m_asset.scriptType))});
    properties.push_back({"fields", NLS::Engine::Serialize::PropertyValue::String(std::move(serializedFields))});
    return true;
}

bool ScriptComponent::DeserializeObjectGraphProperties(
    const std::vector<NLS::Engine::Serialize::PropertyRecord>& properties)
{
    const auto* assetValue = FindProperty(properties, "assetId");
    const auto* languageValue = FindProperty(properties, "language");
    const auto* scriptTypeValue = FindProperty(properties, "scriptTypeId");
    const auto* fieldsValue = FindProperty(properties, "fields");
    if (assetValue && assetValue->GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::String)
        throw std::invalid_argument("ScriptComponent assetId must be a string.");
    if (languageValue && languageValue->GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Integer)
        throw std::invalid_argument("ScriptComponent language must be an integer.");
    if (scriptTypeValue && scriptTypeValue->GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::String)
        throw std::invalid_argument("ScriptComponent scriptTypeId must be a string.");
    if (fieldsValue && fieldsValue->GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::String)
        throw std::invalid_argument("ScriptComponent fields must be a serialized JSON string.");

    const auto assetText = ReadString(properties, "assetId");
    const auto language = ReadInteger(properties, "language");
    const auto scriptTypeText = ReadString(properties, "scriptTypeId");
    const auto fields = ReadString(properties, "fields");
    if (!HasProperty(properties, "assetId") &&
        !HasProperty(properties, "language") &&
        !HasProperty(properties, "scriptTypeId") &&
        !HasProperty(properties, "fields"))
        return false;

    ScriptAsset asset;
    if (assetText && !assetText->empty())
    {
        const auto guid = NLS::Guid::TryParse(*assetText);
        if (!guid.has_value())
            throw std::invalid_argument("ScriptComponent assetId is not a valid Guid.");
        asset.assetId = NLS::Core::Assets::AssetId(*guid);
    }
    if (language)
    {
        if (*language < 0 || *language > static_cast<int64_t>(ScriptLanguage::Lua))
            throw std::invalid_argument("ScriptComponent language is invalid.");
        asset.language = static_cast<ScriptLanguage>(*language);
    }
    if (scriptTypeText && !scriptTypeText->empty())
    {
        uint64_t scriptType = 0;
        const auto conversion = std::from_chars(
            scriptTypeText->data(), scriptTypeText->data() + scriptTypeText->size(), scriptType);
        if (conversion.ec != std::errc{} || conversion.ptr != scriptTypeText->data() + scriptTypeText->size())
            throw std::invalid_argument("ScriptComponent scriptTypeId is invalid.");
        asset.scriptType = scriptType;
    }

    RestoreSerializedState(asset, fields.value_or(std::string("{}")));
    return true;
}
}
