#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Inspector/ComponentEditor.h"
#include "Inspector/InspectorTypes.h"

namespace NLS::Engine::Components { class Component; }

namespace NLS::Editor::Inspector
{
struct InspectorObjectReferencePickerState;
namespace Serialization = NLS::Editor::Serialization;

enum class InspectorDrawerScope : uint8_t
{
    Field,
    ExplicitMetadata,
    ExactType,
    TypeFamily,
    ReadOnlyFallback
};

struct TargetEditorRegistration
{
    InspectorTargetKind targetKind = InspectorTargetKind::None;
    std::string identifier;
};

struct ComponentEditorRegistration
{
    meta::Type inspectedType;
    std::string identifier;
    // Mirrors the external editor's CustomEditor(inspectedType, editorForChildClasses,
    // isFallback) selection rules.
    bool editorForChildClasses = false;
    bool isFallback = false;
    // Mirrors the external editor's [CanEditMultipleObjects]. Custom editors that do not opt
    // in receive a host-level diagnostic for multi-selection.
    bool supportsMultiObject = false;
    std::function<std::unique_ptr<ComponentEditor>()> create;
    // Higher priority wins inside the existing exact/derived/fallback phase.
    // The descriptor registry rejects duplicate target/inheritance/priority
    // registrations before this reaches the resolver.
    int32_t priority = 0;
    // Reflected script type name for an editor that targets a C#/Lua script
    // component. Every script component shares one native reflection type, so
    // such an editor is matched by the script's own type name instead of by the
    // component type. Empty for native component editors.
    std::string scriptTypeName;
};

// Property drawers never receive a raw target pointer. They operate on the
// staged SerializedProperty and commit through its SerializedObject boundary.
struct InspectorPropertyDrawerContext
{
    const PropertyNode& property;
    meta::Variant& value;
    const std::string& widgetId;
    float uiScale = 1.0f;
    bool changed = false;
    std::function<bool(meta::Variant, std::string)> preview;
    std::function<bool()> commit;
    InspectorObjectReferencePickerState* objectReferencePickerState = nullptr;
    // Canonical property cursor for drawers that need to read/write staged
    // values directly. PropertyNode is presentation metadata only.
    Serialization::SerializedProperty* serializedProperty = nullptr;
};

struct InspectorDecoratorDrawerContext
{
    const PropertyNode& property;
    float uiScale = 1.0f;
};

struct PropertyDrawerRegistration
{
    InspectorDrawerScope scope = InspectorDrawerScope::ReadOnlyFallback;
    std::string identifier;
    PropertyPath fieldPath;
    std::string metadataKey;
    meta::Type valueType;
    // Return true after drawing, including when the control was not changed.
    // A false return asks InspectorView to continue with its shared drawer.
    std::function<bool(InspectorPropertyDrawerContext&)> draw;
};

struct DecoratorDrawerRegistration
{
    std::string metadataKey;
    std::string identifier;
    std::function<void(const InspectorDecoratorDrawerContext&)> draw;
};

class InspectorRegistry final
{
public:
    static InspectorRegistry& Get();
    InspectorRegistry();
    InspectorRegistry(const InspectorRegistry&) = delete;
    InspectorRegistry& operator=(const InspectorRegistry&) = delete;


    void RegisterTargetAdapter(std::shared_ptr<IInspectorTargetAdapter> adapter);
    [[nodiscard]] const IInspectorTargetAdapter* ResolveTargetAdapter(const InspectorTargetSet& targets) const;

    void RegisterTargetEditor(TargetEditorRegistration registration);
    void RegisterComponentEditor(ComponentEditorRegistration registration);
    // Removes every component editor whose identifier starts with the given
    // prefix.  The scripted editor registry owns identifiers prefixed with
    // "script:" and re-registers them after a domain reload, so it must be
    // able to drop its own entries without touching native registrations.
    void UnregisterComponentEditorsByIdentifierPrefix(std::string_view prefix);
    void RegisterPropertyDrawer(PropertyDrawerRegistration registration);
    void RegisterDecoratorDrawer(DecoratorDrawerRegistration registration);

    [[nodiscard]] std::optional<PropertyDrawerRegistration> ResolvePropertyDrawer(const PropertyNode& node) const;
    [[nodiscard]] std::vector<DecoratorDrawerRegistration> ResolveDecoratorDrawers(const PropertyNode& node) const;
    [[nodiscard]] std::optional<ComponentEditorRegistration> ResolveComponentEditor(const meta::Type& type) const;
    // Resolves an editor registered for a script component by the script's
    // reflected type name (for example "Nullus.GameScripts.RotatingCube").
    [[nodiscard]] std::optional<ComponentEditorRegistration> ResolveScriptComponentEditor(
        std::string_view scriptTypeName) const;
    [[nodiscard]] uint64_t GetVersion() const;

    [[nodiscard]] uint64_t GetLifecycleId() const;
private:
    [[nodiscard]] bool IsDuplicate(const PropertyDrawerRegistration& registration) const;

    std::vector<std::shared_ptr<IInspectorTargetAdapter>> m_adapters;
    std::vector<TargetEditorRegistration> m_targetEditors;
    std::vector<ComponentEditorRegistration> m_componentEditors;
    std::vector<PropertyDrawerRegistration> m_propertyDrawers;
    std::vector<DecoratorDrawerRegistration> m_decoratorDrawers;
    uint64_t m_version = 1u;
    uint64_t m_lifecycleId = 0u;
};
} // namespace NLS::Editor::Inspector
