#pragma once

#include "ScriptRuntime.h"
#include "ScriptFieldSerialization.h"

#include <Reflection/Macros.h>
#include <Components/Component.h>
#include "ScriptComponent.generated.h"

#include <utility>
#include <vector>

namespace NLS::Engine::Serialize
{
struct PropertyRecord;
}

namespace NLS::Scripting
{
CLASS(NLS_SCRIPTING_API ScriptComponent) : public NLS::Engine::Components::Component
{
public:
    GENERATED_BODY()

    ScriptComponent();
    ~ScriptComponent() override;

    void SetRuntime(ScriptRuntime* runtime);
    ScriptRuntime* GetRuntime() const { return m_runtime; }

    void SetScriptAsset(ScriptAsset asset);
    // Scene serialization stores only the stable AssetId.  The editor calls
    // this after loading the scene to attach the imported source metadata
    // without replacing serialized field overrides.
    void ResolveScriptAsset(ScriptAsset asset);
    const ScriptAsset& GetScriptAsset() const { return m_asset; }
    const SerializedScriptFields& GetSerializedFields() const { return m_fields; }
    SerializedScriptFields& GetSerializedFields() { return m_fields; }
    const OrphanScriptFields& GetOrphanFields() const { return m_orphanFields; }
    OrphanScriptFields& GetOrphanFields() { return m_orphanFields; }
    bool SetSerializedField(ScriptFieldId field, ScriptValue value);
    // Reads the current backend value for an instance field without changing
    // the serialized override map.  The editor uses this while in Play mode
    // so Inspector values reflect script mutations without persisting them.
    bool GetRuntimeSerializedField(ScriptFieldId field, ScriptValue& output) const;
    void RestoreSerializedState(ScriptAsset asset, std::string serializedFields);
    const ScriptTypeDescriptor* GetScriptTypeDescriptor() const;
    const ScriptValue* FindSerializedField(ScriptFieldId field) const
    {
        const auto found = m_fields.find(field);
        return found == m_fields.end() ? nullptr : &found->second;
    }
    ScriptInstanceHandle GetInstance() const { return m_instance; }

    void OnCreate() override;
    void OnAwake() override;
    void OnStart() override;
    void OnEnable() override;
    void OnDisable() override;
    void OnUpdate(float deltaTime) override;
    void OnFixedUpdate(float deltaTime) override;
    void OnLateUpdate(float deltaTime) override;
    void OnDestroy() override;

    bool SerializeObjectGraphProperties(
        std::vector<NLS::Engine::Serialize::PropertyRecord>& properties) const override;
    bool DeserializeObjectGraphProperties(
        const std::vector<NLS::Engine::Serialize::PropertyRecord>& properties) override;

private:
    void Invoke(ScriptCallback callback, float deltaTime);

    ScriptRuntime* m_runtime = nullptr;
    ScriptAsset m_asset;
    ScriptInstanceHandle m_instance;
    SerializedScriptFields m_fields;
    OrphanScriptFields m_orphanFields;
    // Retain the original payload until a runtime/schema is available so
    // alias migration can happen after scene loading.
    std::string m_pendingSerializedFields;
    bool m_destroyed = false;
    bool m_awakeCalled = false;
};
}
