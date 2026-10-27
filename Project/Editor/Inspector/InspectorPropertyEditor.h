#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include "Properties/PropertyControlState.h"

#include "Inspector/InspectorTypes.h"
#include "Inspector/ObjectReferencePicker.h"
#include "Serialization/SerializedObject.h"

namespace NLS::Editor::Inspector
{
struct PropertyDrawResult
{
    bool applied = false;
    bool completed = false;
    bool canceled = false;
    std::vector<std::string> changedPaths;
    bool hostAction = false;
};

// Panels supply a staged stream and their document's apply boundary. Controls
// never retain the context, a target pointer, or a callback past the draw call.
struct PropertyEditorContext
{
    Serialization::SerializedObject* object = nullptr;
    std::function<Serialization::SerializedProperty(const PropertyNode&)> resolve;
    std::function<bool()> apply;
    std::function<void(const PropertyNode&)> changed;
    std::function<void()> structureChanged;
    std::function<void(const PropertyNode&, const char*)> drawOverrideMenu;
    std::function<void(const PropertyNode&)> drawOverrideMenuItems;
    float uiScale = 0.0f;
    float labelColumnLogicalWidth = 0.0f;
    std::function<std::optional<meta::Variant>(const PropertyNode&)> defaultValue;
    std::function<std::optional<meta::Variant>(const PropertyNode&)> inheritedValue;
    std::function<bool(const PropertyNode&)> restoreInherited;
};

enum class PropertyEditorLayout
{
    Properties,
    Compact,
    TableRow,
    ValueOnly
};

// The single renderer for reflected properties, including registered drawers,
// mixed/read-only values, vectors, colors, enums, references and collections.
// Inspector, settings, asset editors and embedded details use this component.
class InspectorPropertyEditor final
{
public:
    InspectorPropertyEditor();
    ~InspectorPropertyEditor();
    InspectorPropertyEditor(const InspectorPropertyEditor&)=delete;
    InspectorPropertyEditor& operator=(const InspectorPropertyEditor&)=delete;

    PropertyDrawResult DrawProperties(const std::vector<PropertyNode>& properties, const PropertyEditorContext& context);
    PropertyDrawResult DrawProperty(const PropertyNode& property, const PropertyEditorContext& context,
                                    PropertyEditorLayout layout = PropertyEditorLayout::ValueOnly);
    PropertyDrawResult FinishItem(const PropertyNode& property, const PropertyEditorContext& context);
    [[nodiscard]] const std::string& GetLastError() const { return m_error; }
    [[nodiscard]] bool HasActiveControls() const { return ImGui::GetCurrentContext()!=nullptr && m_drawFrame == ImGui::GetFrameCount() && m_frameActive; }
    [[nodiscard]] bool HasInvalidatedDrawers() const;
    void ClearControlState()
    {
        m_expansionOverrides.clear();
        m_controlStateInvalidated=true;
        if(!m_context) {m_controlStates.clear();m_objectReferencePickerState.searchTextByKey.clear();m_controlStateInvalidated=false;}
        m_frameActive=false;
    }

private:
    class ScopedContext;
    void DrawPropertiesImpl(const std::vector<PropertyNode>& properties);
    void DrawPropertyCompact(const PropertyNode& property);
    void DrawPropertyRow(const PropertyNode& property);
    void DrawPropertyDecorators(const PropertyNode& property);
    void DrawPropertyLabel(const PropertyNode& property, float availableWidth);
    void DrawPrefabOverrideMenu(const PropertyNode& property, const char* popupId);
    bool DrawPropertyValue(const PropertyNode& property);
    bool DrawRegisteredValue(const PropertyNode& property, meta::Variant& value, const std::string& id, bool& completed);
    bool DrawObjectReferenceValue(const PropertyNode& property, meta::Variant& value, const std::string& id, bool& completed);
    UI::Controls::ControlResult DrawSerializedStruct(const PropertyNode& property);
    void DrawPropertyMenu(const PropertyNode& property);
    void DrawPropertyMenuItems(const PropertyNode& property, bool clipboardItems);
    bool IsPropertyVisible(const PropertyNode& property) const;
    Serialization::SerializedProperty ResolveSerializedProperty(const PropertyNode& property) const;
    bool PreviewSerializedProperty(const PropertyNode& property, meta::Variant value, std::string label,uint64_t components=~0ull);
    bool CommitSerializedEdit();
    void CommitOrCancelAfterItem(const PropertyNode& property);
    void NotifyPropertyChanged(const PropertyNode& property);
    float GetUiScale() const;
    template<class T>
    bool PreviewValue(const PropertyNode& property, const T& value, std::string label)
    {
        return PreviewSerializedProperty(property, meta::Variant(value), std::move(label));
    }

    const PropertyEditorContext* m_context = nullptr;
    PropertyDrawResult* m_result = nullptr;
    InspectorObjectReferencePickerState m_objectReferencePickerState;
    std::string m_error;
    bool m_structureChanged = false;
    UI::Controls::ControlResult m_interaction;
    std::unordered_map<ImGuiID, Properties::PropertyControlState> m_controlStates;
    std::unordered_map<std::string,bool> m_expansionOverrides;
    uint64_t m_drawerVersion = 0;
    int m_drawFrame = -1;
    bool m_frameActive = false;
    uint64_t m_drawerListener=0;
    std::shared_ptr<const void> m_listenerLifetime=std::make_shared<int>();
    std::weak_ptr<const void> m_registryLifetime;
    bool m_controlStateInvalidated=false;

};
} // namespace NLS::Editor::Inspector
