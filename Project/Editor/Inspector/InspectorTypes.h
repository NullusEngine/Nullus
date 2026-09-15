#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Object/Object.h>
#include <Reflection/Type.h>
#include <Reflection/Variant.h>
#include <Serialize/PPtr.h>

namespace NLS::Editor::Inspector
{
enum class InspectorTargetKind : uint8_t
{
    None,
    GameObject,
    NativeObject,
    ScriptObject,
    AssetImporter,
    Material,
    ProjectSettings
};

/**
 * Stable selection identity.  Object-backed targets are resolved through the
 * instance registry when used; asset-backed targets are resolved from their
 * persistent id/path.  No inspector model object owns a live engine pointer.
 */
struct InspectorTargetHandle
{
    InspectorTargetKind kind = InspectorTargetKind::None;
    // Commands retain ObjectIdentifier, not a runtime slot. instanceId remains
    // a draw-frame cache for legacy callers that have not yet captured one.
    Engine::Serialize::ObjectIdentifier objectIdentifier;
    InstanceID instanceId = InstanceID_None;
    std::string assetId;
    std::string resourcePath;
    std::string typeName;

    [[nodiscard]] bool IsValid() const;
    [[nodiscard]] bool IsObjectTarget() const;
    [[nodiscard]] bool operator==(const InspectorTargetHandle& other) const;
    [[nodiscard]] bool operator!=(const InspectorTargetHandle& other) const
    {
        return !(*this == other);
    }

    static InspectorTargetHandle FromObject(const Object& object, InspectorTargetKind kind = InspectorTargetKind::NativeObject);
    static InspectorTargetHandle FromAsset(
        InspectorTargetKind kind,
        std::string resourcePath,
        std::string assetId = {},
        std::string typeName = {});
};

struct InspectorTargetSet
{
    std::vector<InspectorTargetHandle> targets;
    uint64_t revision = 0u;

    [[nodiscard]] bool Empty() const { return targets.empty(); }
    [[nodiscard]] bool IsSingleTarget() const { return targets.size() == 1u; }
    [[nodiscard]] const InspectorTargetHandle* Primary() const
    {
        return IsSingleTarget() ? &targets.front() : nullptr;
    }

    void Normalize();
    [[nodiscard]] bool operator==(const InspectorTargetSet& other) const;
    [[nodiscard]] bool operator!=(const InspectorTargetSet& other) const
    {
        return !(*this == other);
    }
};

class PropertyPath
{
public:
    PropertyPath() = default;
    explicit PropertyPath(std::string value)
        : m_value(std::move(value))
    {
    }

    [[nodiscard]] bool Empty() const { return m_value.empty(); }
    [[nodiscard]] const std::string& ToString() const { return m_value; }
    [[nodiscard]] PropertyPath Field(std::string_view name) const;
    [[nodiscard]] PropertyPath Index(size_t index) const;
    [[nodiscard]] PropertyPath ArraySize() const;
    [[nodiscard]] PropertyPath ArrayElement(size_t index) const;
    [[nodiscard]] bool IsArraySize() const;
    [[nodiscard]] bool IsArrayElement() const;

    [[nodiscard]] bool operator==(const PropertyPath& other) const { return m_value == other.m_value; }
    [[nodiscard]] bool operator!=(const PropertyPath& other) const { return !(*this == other); }
    [[nodiscard]] bool operator<(const PropertyPath& other) const { return m_value < other.m_value; }

private:
    std::string m_value;
};

enum class InspectorPropertyKind : uint8_t
{
    Unsupported,
    Boolean,
    SignedInteger8,
    UnsignedInteger8,
    SignedInteger16,
    UnsignedInteger16,
    SignedInteger32,
    UnsignedInteger32,
    SignedInteger64,
    UnsignedInteger64,
    Float,
    Double,
    Enum,
    Flags,
    String,
    Choice,
    Vector2,
    Vector3,
    Vector4,
    Quaternion,
    Color,
    Rect,
    Vector2Int,
    Vector3Int,
    RectInt,
    Bounds,
    BoundsInt,
    BoundingSphere,
    LayerMask,
    Struct,
    Array,
    ObjectReference,
    ScriptValue
};

enum class PropertyValueState : uint8_t
{
    Valid,
    Mixed,
    Invalid
};

enum class PrefabOverrideState : uint8_t
{
    None,
    PropertyOverride,
    StructuralOverride,
    Invalid
};

struct EditorPropertyMetadata
{
    std::string displayName;
    std::string tooltip;
    std::string header;
    float spaceBefore = 0.0f;
    std::optional<float> rangeMin;
    std::optional<float> rangeMax;
    std::optional<float> min;
    std::optional<float> max;
    bool multiline = false;
    bool readOnly = false;
    bool hideInInspector = false;
    bool flags = false;
    bool colorUsage = false;
    std::string referenceTypeConstraint;
    // Optional explicit PropertyDrawer key. This is the engine-style escape
    // hatch for a field whose storage type needs a domain-specific editor.
    std::string customDrawer;
};

struct InspectorPropertyChoice
{
    std::string value;
    std::string label;
    std::string tooltip;
    bool enabled = true;
    // Optional typed value used by Choice drawers for integer/enum-backed
    // properties. Legacy string choices leave this invalid.
    meta::Variant typedValue;
};

struct PropertyNode
{
    PropertyPath path;
    std::string fieldName;
    uint64_t fieldId = 0u;
    std::vector<std::string> aliases;
    std::string displayName;
    std::string diagnostic;
    InspectorPropertyKind kind = InspectorPropertyKind::Unsupported;
    meta::Type valueType;
    EditorPropertyMetadata metadata;
    std::vector<InspectorPropertyChoice> choices;
    PropertyValueState valueState = PropertyValueState::Valid;
    PrefabOverrideState prefabOverride = PrefabOverrideState::None;
    InspectorTargetHandle target;
    // Pure presentation identity. Targets must never be recovered from the
    // legacy getter/setter binding registry.
    std::vector<InspectorTargetHandle> targets;
    bool readOnly = false;
    bool isComponent = false;
    bool isTransform = false;
    bool isFixedBuffer = false;
    size_t fixedBufferSize = 0u;
    bool expandedByDefault = true;
    uint64_t structuralVersion = 0u;

    std::vector<PropertyNode> children;
};

// Disposable Inspector chrome and UI projection. This is deliberately not a
// serialization model: every field value and mutation is resolved through a
// SerializedObject/SerializedProperty cursor.
struct InspectorPresentation
{
    InspectorTargetSet targets;
    uint64_t structureVersion = 0u;
    std::vector<PropertyNode> roots;
    // Properties rendered by a host-owned chrome surface (for example the
    // GameObject header) still participate in SerializedObject snapshots, but
    // are intentionally excluded from the body/root projection returned to
    // component editors.  They are queried by their stable PropertyPath.
    std::vector<PropertyNode> auxiliary;
};

[[nodiscard]] std::vector<InspectorTargetHandle> GetPropertyNodeTargets(const PropertyNode& node);
void SetPropertyNodeTargets(
    PropertyNode& node,
    std::vector<InspectorTargetHandle> targets);

struct InspectorBuildContext
{
    std::string searchText;
    bool includeHidden = false;
};

enum class InspectorTargetActionKind : uint8_t
{
    Apply,
    Revert,
    Reimport,
    Save,
    Reload,
    Preview
};

struct InspectorTargetAction
{
    InspectorTargetActionKind kind = InspectorTargetActionKind::Apply;
    std::string label;
    std::string tooltip;
    bool enabled = true;
    bool primary = false;
};

class IInspectorTargetAdapter
{
public:
    virtual ~IInspectorTargetAdapter() = default;

    [[nodiscard]] virtual bool CanBuild(const InspectorTargetSet& targets) const = 0;
    [[nodiscard]] virtual uint64_t GetStructureVersion(const InspectorTargetSet& targets) const = 0;
    // Target adapters own only the Inspector chrome (title, component cards,
    // auxiliary host controls) and target actions. Field rows are projected by
    // InspectorProjectionBuilder from the canonical SerializedObject.
    virtual InspectorPresentation BuildPresentation(
        const InspectorTargetSet& targets,
        const InspectorBuildContext& context) const = 0;

    // Asset-backed editors expose their commit actions here. Native object
    // editors intentionally use the common edit session and return no actions.
    [[nodiscard]] virtual std::vector<InspectorTargetAction> GetActions(const InspectorTargetSet&) const
    {
        return {};
    }
    virtual bool ExecuteAction(const InspectorTargetSet&, InspectorTargetActionKind, std::string& error) const
    {
        error = "This Inspector target does not support that action.";
        return false;
    }
};

[[nodiscard]] bool IsIntegerPropertyKind(InspectorPropertyKind kind);
[[nodiscard]] bool IsFloatingPointPropertyKind(InspectorPropertyKind kind);
[[nodiscard]] bool IsNumericPropertyKind(InspectorPropertyKind kind);
} // namespace NLS::Editor::Inspector
