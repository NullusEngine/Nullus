#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Inspector/InspectorTypes.h"
#include "Serialization/SerializedObject.h"

namespace NLS::Editor::Inspector
{
class InspectorView;

// Component editors use the editor-level canonical serialization core directly.
// There is no Inspector-private SerializedObject implementation or implicit
// live-write mode: all values are staged on the shared SerializedObject.

struct ComponentEditorPropertyOptions
{
    // Empty values preserve reflected metadata.  Custom editors should only
    // override presentation or semantic drawer selection; storage and writes
    // remain owned by SerializedProperty.
    std::string label;
    std::string tooltip;
    std::optional<InspectorPropertyKind> kind;
    std::vector<InspectorPropertyChoice> choices;
    bool includeChildren = true;
    bool forceVisible = true;
};

struct ComponentEditorAction
{
    std::string label;
    std::string tooltip;
    std::string searchText;
    bool enabled = true;
    std::function<void()> execute;
};

enum class ComponentEditorMessageKind : uint8_t
{
    Info,
    Warning,
    Error
};

struct ComponentEditorMessage
{
    ComponentEditorMessageKind kind = ComponentEditorMessageKind::Info;
    std::string text;
};

// Host callbacks are intentionally synchronous.  PropertyField, Button and
// HelpBox are drawn at the exact point the custom editor calls them; there is
// no deferred command buffer whose ordering can diverge from OnInspectorGUI.
// InspectorView supplies these callbacks so all controls still share the
// common drawer, search, layout, object-picker and undo implementation.
struct ComponentEditorContextCallbacks
{
    std::function<bool(
        const Serialization::SerializedProperty&,
        const ComponentEditorPropertyOptions&)> drawProperty;
    std::function<bool(const ComponentEditorAction&)> drawButton;
    std::function<void(const ComponentEditorMessage&)> drawHelpBox;
    std::function<bool()> applyModifiedProperties;
    std::function<bool()> discardModifiedProperties;
};

class ComponentEditorContext final
{
public:
    ComponentEditorContext(
        Serialization::SerializedObject& serializedObject,
        InspectorTargetSet targets,
        std::string_view searchText,
        ComponentEditorContextCallbacks callbacks = {});
    ~ComponentEditorContext();

    [[nodiscard]] Serialization::SerializedObject& GetSerializedObject() { return m_serializedObject; }
    [[nodiscard]] const Serialization::SerializedObject& GetSerializedObject() const { return m_serializedObject; }
    [[nodiscard]] const InspectorTargetSet& GetTargets() const { return m_targets; }
    // the external editor's target is the first object even when targets[] contains several
    // objects; custom editors that need a single-object operation should check
    // IsMultiObject() explicitly.
    [[nodiscard]] const InspectorTargetHandle* GetTarget() const
    {
        return m_targets.targets.empty() ? nullptr : &m_targets.targets.front();
    }
    [[nodiscard]] std::string_view GetSearchText() const { return m_searchText; }
    [[nodiscard]] bool IsMultiObject() const { return !m_targets.IsSingleTarget(); }

    bool DrawDefaultInspector();
    bool PropertyField(
        const Serialization::SerializedProperty& property,
        ComponentEditorPropertyOptions options = {});
    bool PropertyField(
        std::string_view propertyPath,
        ComponentEditorPropertyOptions options = {});
    bool Button(
        std::string label,
        std::function<void()> execute = {},
        bool enabled = true,
        std::string tooltip = {},
        std::string searchText = {});
    void HelpBox(std::string text, ComponentEditorMessageKind kind = ComponentEditorMessageKind::Info);

    [[nodiscard]] bool ApplyModifiedProperties();
    [[nodiscard]] bool DiscardModifiedProperties();

    [[nodiscard]] static bool MatchesSearch(
        std::string_view displayName,
        std::string_view fieldName,
        std::string_view path,
        std::string_view searchText);

private:
    [[nodiscard]] bool MatchesSearch(const Serialization::SerializedProperty& property) const;

    Serialization::SerializedObject& m_serializedObject;
    InspectorTargetSet m_targets;
    std::string m_searchText;
    ComponentEditorContextCallbacks m_callbacks;
};

// Base class for a native component editor. The host owns the
// serialized-object update and installs a frame-scoped context before calling
// the parameterless OnInspectorGUI().  Derived editors use the protected
// helpers below, which keeps their code close to the external editor's
// serializedObject.FindProperty / EditorGUILayout.PropertyField style.
class ComponentEditor
{
public:
    virtual ~ComponentEditor() = default;

    virtual void OnEnable() {}
    virtual void OnDisable() {}
    virtual void OnInspectorGUI() = 0;

    void UpdateSerializedObject(const PropertyNode& componentNode);
    [[nodiscard]] Serialization::SerializedObject& GetSerializedObject() { return m_serializedObject; }
    [[nodiscard]] const Serialization::SerializedObject& GetSerializedObject() const { return m_serializedObject; }

private:
    enum class BoundBackendKind : uint8_t
    {
        None,
        Accessor,
        NativeReflection
    };

    void BeginInspectorGUI(ComponentEditorContext& context) { m_context = &context; }
    void EndInspectorGUI() { m_context = nullptr; }

protected:
    [[nodiscard]] Serialization::SerializedObject& serializedObject() { return m_serializedObject; }
    [[nodiscard]] const Serialization::SerializedObject& serializedObject() const { return m_serializedObject; }
    [[nodiscard]] const InspectorTargetSet& targets() const;
    [[nodiscard]] const InspectorTargetHandle* target() const;
    // The Inspector search text for the frame that is currently drawing this
    // editor.  Empty outside OnInspectorGUI.
    [[nodiscard]] std::string_view SearchText() const;

    bool DrawDefaultInspector();
    bool PropertyField(
        const Serialization::SerializedProperty& property,
        ComponentEditorPropertyOptions options = {});
    bool PropertyField(
        std::string_view propertyPath,
        ComponentEditorPropertyOptions options = {});
    bool Button(
        std::string label,
        std::function<void()> execute = {},
        bool enabled = true,
        std::string tooltip = {},
        std::string searchText = {});
    void HelpBox(std::string text, ComponentEditorMessageKind kind = ComponentEditorMessageKind::Info);
    [[nodiscard]] bool ApplyModifiedProperties();
    [[nodiscard]] bool DiscardModifiedProperties();

private:
    Serialization::SerializedObject m_serializedObject;
    ComponentEditorContext* m_context = nullptr;
    // Snapshot of the binding produced by the last UpdateSerializedObject()
    // call.  The Inspector rebinds component editors every frame, so comparing
    // against this snapshot keeps a multi-frame staged preview alive until it
    // is committed or discarded.
    InspectorTargetSet m_boundTargets;
    BoundBackendKind m_boundBackend = BoundBackendKind::None;
    friend class InspectorView;
};

class DefaultComponentEditor final : public ComponentEditor
{
public:
    void OnInspectorGUI() override;
};

} // namespace NLS::Editor::Inspector
