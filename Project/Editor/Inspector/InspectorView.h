#pragma once

#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Eventing/Event.h>
#include <UI/Widgets/AWidget.h>

#include "Inspector/ComponentStructureUndo.h"
#include "Inspector/ComponentEditor.h"
#include "Inspector/InspectorUndo.h"
#include "Inspector/InspectorTypes.h"
#include "Inspector/ObjectReferencePicker.h"
#include "Inspector/PrefabInspectorSupport.h"
#include "Panels/ComponentSearchPanel.h"

namespace NLS::Engine { class GameObject; }

namespace NLS::Editor::Inspector
{
class InspectorView final : public UI::Widgets::AWidget
{
public:
    InspectorView();
    ~InspectorView() override;

    void SetTargets(InspectorTargetSet targets);
    [[nodiscard]] const InspectorTargetSet& GetTargets() const;
    void Refresh();
    void SetSearchText(std::string searchText);
    void SetToolbarVisible(bool visible);

    void SetLocked(bool locked);
    [[nodiscard]] bool IsLocked() const;
    [[nodiscard]] Engine::GameObject* GetPrimaryGameObject() const;

    Event<const PropertyNode&> PropertyChangedEvent;
    Event<> StructureChangedEvent;

protected:
    void _Draw_Impl() override;

private:
    void EnsureTree();
    void DrawToolbar();
    void DrawGameObject(const PropertyNode& gameObjectNode);
    void DrawComponent(const PropertyNode& componentNode, size_t componentIndex);
    void DrawComponentEditorBody(const PropertyNode& componentNode);
    void DrawProperties(const std::vector<PropertyNode>& properties);
    void DrawPropertyCompact(const PropertyNode& property);
    void DrawPropertyRow(const PropertyNode& property);
    void DrawPropertyDecorators(const PropertyNode& property);
    void DrawPropertyLabel(const PropertyNode& property, float availableWidth);
    void DrawPrefabOverrideMenu(const PropertyNode& property, const char* popupId);
    bool DrawPropertyValue(const PropertyNode& property);
    bool DrawObjectReferenceValue(const PropertyNode& property, meta::Variant& value, const std::string& id);
    bool DrawLooseValue(meta::Variant& value, const meta::Type& type, const std::string& id, int depth = 0);
    bool DrawArrayValue(meta::Variant& value, const meta::Type& arrayType, const std::string& id, int depth);
    bool DrawStructValue(meta::Variant& value, const meta::Type& type, const std::string& id, int depth);
    void DrawAddComponent();
    void DrawComponentMenu(const PropertyNode& componentNode, size_t componentIndex);
    void DrawTargetActions();
    void DrawMissingTarget();
    void RefreshPrefabOverridesIfNeeded();
    void CommitOrCancelAfterItem(const PropertyNode& property);
    void NotifyPropertyChanged(const PropertyNode& property);
    [[nodiscard]] Serialization::SerializedProperty ResolveSerializedProperty(const PropertyNode& property) const;
    bool PreviewSerializedProperty(
        const PropertyNode& property,
        meta::Variant value,
        std::string actionLabel);
    bool CommitSerializedEdit();
    [[nodiscard]] float GetUiScale() const;
    [[nodiscard]] bool UseCompactLayout() const;
    [[nodiscard]] std::string GetComponentTitle(const PropertyNode& componentNode) const;
    void ClearComponentEditors();
    void PruneComponentEditors();

    template <typename TValue>
    bool PreviewValue(const PropertyNode& property, const TValue& value, std::string actionLabel);

private:
    InspectorTargetSet m_targets;
    InspectorPresentation m_tree;
    std::unique_ptr<Serialization::SerializedObject> m_serializedObject;
    uint64_t m_treeStructureVersion = 0u;
    uint64_t m_registryVersion = 0u;
    uint64_t m_metadataVersion = 0u;
    bool m_treeDirty = true;
    bool m_locked = false;
    bool m_toolbarVisible = true;
    bool m_prefabOverridesDirty = true;
    std::string m_searchText;
    float m_labelColumnLogicalWidth = 0.0f;
    ListenerID m_selectionListener = InvalidListenerID;
    ListenerID m_componentAddedListener = InvalidListenerID;
    Panels::ComponentSearchPanel m_componentPicker;
    std::optional<meta::Variant> m_arrayClipboard;
    std::string m_arrayClipboardType;
    std::optional<ComponentSnapshot> m_componentClipboard;
    InspectorObjectReferencePickerState m_objectReferencePickerState;
    std::string m_targetActionError;
    std::string m_prefabActionError;
    std::vector<PrefabInspectorAction> m_prefabActions;

    struct ComponentEditorEntry
    {
        std::string registrationIdentifier;
        meta::Type componentType;
        std::vector<InspectorTargetHandle> targets;
        std::unique_ptr<ComponentEditor> editor;
    };
    std::unordered_map<std::string, ComponentEditorEntry> m_componentEditors;
    std::unordered_set<std::string> m_liveComponentEditorKeys;

    // Component editors pass an actual SerializedProperty to the common UI
    // drawer. This transient draw-only override selects that editor's native
    // stream during the synchronous DrawProperty call.
    struct SerializedDrawOverride
    {
        Serialization::SerializedProperty property;
        Serialization::SerializedObject* owner = nullptr;
    };
    std::optional<SerializedDrawOverride> m_serializedDrawOverride;
};
} // namespace NLS::Editor::Inspector
