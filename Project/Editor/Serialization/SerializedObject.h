#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Reflection/Variant.h>

#include "Inspector/InspectorTypes.h"
#include "Serialize/NativeSerializedStream.h"
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Serialization
{
class SerializedProperty;
class SerializedEditSession;

// Resolves an ExposedReference name relative to a stable SerializedObject
// context. Implementations return durable handles only; raw Object pointers
// are allowed only while resolving on the editor thread.
class IExposedReferenceResolver
{
public:
    virtual ~IExposedReferenceResolver() = default;

    [[nodiscard]] virtual std::optional<Inspector::InspectorTargetHandle> Resolve(
        const Inspector::InspectorTargetHandle& context,
        std::string_view exposedName) const = 0;

    // Mirrors the external editor's IExposedPropertyTable write path.  A resolver owns the
    // context-specific override; returning false tells SerializedObject to use
    // the serialized default value instead.  The method is const so resolvers
    // can remain shared immutable services while mutating their protected table.
    virtual bool Set(
        const Inspector::InspectorTargetHandle&,
        std::string_view,
        std::optional<Inspector::InspectorTargetHandle>) const
    {
        return false;
    }
};

// Editor-wide resolver used by native and managed SerializedObject instances
// that were not constructed with a specialised backend resolver. Keys are the
// durable context identity plus the exposed name; no raw Object pointer is
// retained after Bind returns.
class EditorExposedReferenceResolver final : public IExposedReferenceResolver
{
public:
    void Bind(
        const Inspector::InspectorTargetHandle& context,
        std::string exposedName,
        Inspector::InspectorTargetHandle target);
    void Unbind(const Inspector::InspectorTargetHandle& context, std::string_view exposedName);
    void ClearContext(const Inspector::InspectorTargetHandle& context);

    [[nodiscard]] std::optional<Inspector::InspectorTargetHandle> Resolve(
        const Inspector::InspectorTargetHandle& context,
        std::string_view exposedName) const override;
    bool Set(
        const Inspector::InspectorTargetHandle& context,
        std::string_view exposedName,
        std::optional<Inspector::InspectorTargetHandle> target) const override;

private:
    [[nodiscard]] static std::string MakeKey(
        const Inspector::InspectorTargetHandle& context,
        std::string_view exposedName);

    mutable std::mutex m_mutex;
    mutable std::unordered_map<std::string, Inspector::InspectorTargetHandle> m_entries;
};

[[nodiscard]] EditorExposedReferenceResolver& GetEditorExposedReferenceResolver();

// Stores the editor inspection mode while keeping the
// canonical enum in the native serialization namespace.  Inspector and
// managed bindings both consume this value; it must not depend on a UI-only
// type.
enum class SerializedInspectorMode : uint8_t
{
    Normal,
    Debug,
    DebugInternal
};

using SerializedTargetHandle = Inspector::InspectorTargetHandle;
using SerializedTargetSet = Inspector::InspectorTargetSet;
using SerializedPropertyPath = Inspector::PropertyPath;

struct SerializedVersionStamp
{
    uint64_t dataGeneration = 0u;
    uint64_t schemaGeneration = 0u;
    uint64_t structureGeneration = 0u;

    [[nodiscard]] bool operator==(const SerializedVersionStamp& other) const noexcept
    {
        return dataGeneration == other.dataGeneration &&
            schemaGeneration == other.schemaGeneration &&
            structureGeneration == other.structureGeneration;
    }
};

struct SerializedObjectReference
{
    std::string valuePath;
    Engine::Serialize::ObjectIdentifier identifier;
    std::string resourcePath;
    std::string typeName;
    bool isNull = false;

    [[nodiscard]] bool operator==(const SerializedObjectReference& other) const
    {
        return identifier == other.identifier &&
            valuePath == other.valuePath &&
            resourcePath == other.resourcePath &&
            typeName == other.typeName &&
            isNull == other.isNull;
    }
};

// Values own their serialized representation. The native byte stream is the
// only durable form retained by snapshots and Undo commands; draw-time
// Variants are decoded on demand and never become an alternate storage format.
struct SerializedValue
{
    std::vector<uint8_t> bytes;
    // Exact slot order used by the native byte stream.  `references` is kept as
    // optional diagnostic metadata for UI/tests; it is never used to decode or
    // apply a value.
    std::vector<Engine::Serialize::SerializedReferenceEntry> byteReferences;
    std::vector<SerializedObjectReference> references;
    std::string typeName;
    uint64_t typeSignature = 0u;
    bool valid = false;

    [[nodiscard]] bool IsValid() const { return valid; }
    [[nodiscard]] bool Equivalent(const SerializedValue& other) const;
    [[nodiscard]] static std::optional<SerializedValue> Capture(const meta::Variant& value);
    [[nodiscard]] std::optional<meta::Variant> Resolve(const meta::Type& type) const;
};

struct SerializedSchemaNode
{
    SerializedPropertyPath path;
    std::string fieldName;
    uint64_t fieldId = 0u;
    std::vector<std::string> aliases;
    std::string displayName;
    std::string tooltip;
    std::string exactTypeName;
    uint64_t exactTypeId = 0u;
    Inspector::EditorPropertyMetadata metadata;
    int32_t propertyType = -1;
    uint8_t numericType = 0u;
    bool readOnly = false;
    bool isArray = false;
    bool isFixedBuffer = false;
    size_t fixedBufferSize = 0u;
    std::vector<SerializedSchemaNode> children;
};

struct SerializedSchema
{
    std::vector<SerializedSchemaNode> roots;
    uint64_t version = 0u;
};

// A committed edit stores complete before/after byte streams for each stable
// target. The stream owns its TypeTree and reference table, so replay never
// depends on an Inspector UI object or a live Variant.
struct SerializedTargetStreamChange
{
    SerializedTargetHandle target;
    std::shared_ptr<Engine::Serialize::SerializedByteStream> before;
    std::shared_ptr<Engine::Serialize::SerializedByteStream> after;
};

struct SerializedStreamChangeSet
{
    std::vector<SerializedPropertyPath> modifiedPaths;
    std::vector<SerializedTargetStreamChange> targets;

    [[nodiscard]] bool Empty() const { return targets.empty(); }
    [[nodiscard]] size_t EstimatedBytes() const;
};

struct ValidationResult
{
    bool valid = true;
    std::string error;
    [[nodiscard]] static ValidationResult Success() { return {}; }
    [[nodiscard]] static ValidationResult Failure(std::string message)
    {
        return {false, std::move(message)};
    }
};

struct ApplyResult
{
    bool succeeded = true;
    std::string error;
    bool asynchronous = false;
    [[nodiscard]] static ApplyResult Success() { return {}; }
    [[nodiscard]] static ApplyResult Failure(std::string message)
    {
        return {false, std::move(message), false};
    }
};

struct PrefabPropertyState
{
    Inspector::PrefabOverrideState state = Inspector::PrefabOverrideState::None;
    bool defaultOverride = false;
    bool instantiatedPrefab = false;
};

// Backends own the stable target resolution and byte-stream read/write entry
// points. There is deliberately no parallel schema/snapshot/delta contract:
// SerializedObject always validates and replays the same stream representation.
class ISerializedTargetBackend
{
public:
    virtual ~ISerializedTargetBackend() = default;

    virtual NLS::Engine::Serialize::TypeTreeResult BuildTypeTree(
        const SerializedTargetSet&,
        SerializedInspectorMode) const = 0;

    virtual NLS::Engine::Serialize::StreamResult SerializeTarget(
        const SerializedTargetHandle&,
        std::shared_ptr<const NLS::Engine::Serialize::SerializedTypeTree>,
        NLS::Engine::Serialize::SerializedReferenceTable&) const = 0;

    virtual ValidationResult ValidateAndNormalize(
        const SerializedTargetHandle&,
        std::span<const SerializedPropertyPath>,
        NLS::Engine::Serialize::SerializedByteStream&) const = 0;

    virtual ApplyResult DeserializeTarget(
        const SerializedTargetHandle&,
        const NLS::Engine::Serialize::SerializedByteStream&,
        const Undo::UndoExecutionContext&) const = 0;
    virtual SerializedVersionStamp GetVersion(
        const SerializedTargetHandle&) const = 0;
    virtual PrefabPropertyState QueryPrefabState(
        const SerializedTargetHandle&,
        const SerializedPropertyPath&) const
    {
        return {};
    }
};

class SerializedObject : public std::enable_shared_from_this<SerializedObject>
{
public:
    struct State;

    SerializedObject() = default;
    SerializedObject(
        SerializedTargetSet targets,
        std::shared_ptr<ISerializedTargetBackend> backend,
        SerializedInspectorMode mode = SerializedInspectorMode::Normal,
        std::optional<SerializedTargetHandle> context = std::nullopt,
        std::shared_ptr<const IExposedReferenceResolver> exposedReferenceResolver = {});
    ~SerializedObject();

    SerializedObject(const SerializedObject&) = delete;
    SerializedObject& operator=(const SerializedObject&) = delete;

    // Rebinds an editor instance to a new stable target set without recreating
    // a UI-side property tree. Component editors use this during selection
    // changes so their SerializedObject remains canonical stream state.
    void Reset(
        SerializedTargetSet targets,
        std::shared_ptr<ISerializedTargetBackend> backend,
        SerializedInspectorMode mode = SerializedInspectorMode::Normal,
        std::optional<SerializedTargetHandle> context = std::nullopt,
        std::shared_ptr<const IExposedReferenceResolver> exposedReferenceResolver = {});

    void Update();
    [[nodiscard]] bool UpdateIfRequiredOrScript();
    // Retained under the historical name for callers ported from the external
    // editor; the external API marks this spelling obsolete in favour of
    // UpdateIfRequiredOrScript().
    [[nodiscard]] bool UpdateIfDirtyOrScript();

    [[nodiscard]] bool IsValid() const;
    [[nodiscard]] uint64_t GetGeneration() const;
    [[nodiscard]] SerializedVersionStamp GetVersion() const;
    [[nodiscard]] const NLS::Engine::Serialize::SerializedByteStream* GetPrimaryByteStream() const;
    [[nodiscard]] std::vector<SerializedTargetHandle> GetTargetHandles() const;
    [[nodiscard]] std::vector<SerializedTargetHandle> GetTargetObjects() const { return GetTargetHandles(); }
    [[nodiscard]] const SerializedTargetSet& GetTargetSet() const;
    [[nodiscard]] const SerializedTargetHandle* GetTargetObject() const;
    [[nodiscard]] const SerializedTargetHandle* GetContext() const;
    void SetContext(std::optional<SerializedTargetHandle> context);
    [[nodiscard]] std::optional<SerializedTargetHandle> ResolveExposedReference(
        const SerializedProperty& property) const;
    // Equivalent to the external editor's SerializedProperty.exposedReferenceValue setter:
    // with a context resolver the value belongs to that table, otherwise the
    // serialized ExposedReference default is staged in this object.
    [[nodiscard]] bool SetResolvedExposedReference(
        const SerializedProperty& property,
        std::optional<SerializedTargetHandle> target);

    [[nodiscard]] SerializedProperty FindProperty(std::string_view path) const;
    [[nodiscard]] SerializedProperty GetIterator() const;
    [[nodiscard]] SerializedEditSession BeginEditSession(std::string_view path);
    [[nodiscard]] bool CopyFromSerializedProperty(const SerializedProperty& source);
    [[nodiscard]] bool CopyFromSerializedPropertyIfDifferent(const SerializedProperty& source);
    [[nodiscard]] const SerializedSchema& GetSchema() const;

    [[nodiscard]] bool HasModifiedProperties() const;
    [[nodiscard]] bool ApplyModifiedProperties();
    [[nodiscard]] bool ApplyModifiedPropertiesWithoutUndo();
    void DiscardModifiedProperties();
    void SetIsDifferentCacheDirty() const;

    [[nodiscard]] bool IsEditingMultipleObjects() const;
    [[nodiscard]] size_t GetMaxArraySizeForMultiEditing() const;
    void SetMaxArraySizeForMultiEditing(size_t maxSize);
    void SetInspectorMode(SerializedInspectorMode mode);
    [[nodiscard]] SerializedInspectorMode GetInspectorMode() const;
    [[nodiscard]] std::string GetLastError() const;

private:
    friend class SerializedProperty;
    friend class SerializedEditSession;

    std::shared_ptr<State> m_state;
    std::shared_ptr<const void> m_lifetime=std::make_shared<int>();
};

} // namespace NLS::Editor::Serialization

#include "Serialization/SerializedProperty.h"

namespace NLS::Editor::Serialization
{
// A continuous Inspector edit (slider, drag, gizmo) owns one before snapshot
// and commits through the same SerializedObject Apply boundary. Preview may be
// called repeatedly; Cancel restores the staged/live preview and never adds a
// history entry.
class SerializedEditSession final
{
public:
    SerializedEditSession() = default;
    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] const SerializedProperty& GetProperty() const { return m_property; }
    bool Preview(const meta::Variant& value);
    bool Commit();
    bool Cancel();

private:
    SerializedEditSession(SerializedObject* owner, SerializedProperty property);
    SerializedObject* m_owner = nullptr;
    SerializedProperty m_property;
    std::weak_ptr<SerializedObject::State> m_ownerState;
    std::weak_ptr<const void> m_ownerLifetime;
    uint64_t m_updateEpoch=0, m_dataRevision=0, m_schemaGeneration=0;
    bool m_active = false;

    friend class SerializedObject;
};
} // namespace NLS::Editor::Serialization
