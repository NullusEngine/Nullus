#include "Inspector/InspectorPropertyEditor.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include "Properties/CollectionPropertyDrawer.h"

#include <Components/Component.h>
#include <Debug/Logger.h>
#include <GameObject.h>
#include <LayerMask.h>
#include <Math/ExternalReflection.h>
#include <Math/Quaternion.h>
#include <Math/Matrix4.h>
#include <Math/Vector2.h>
#include <Math/Vector3.h>
#include <Math/Vector4.h>
#include <Reflection/ArrayWrapper.h>
#include <Reflection/ReflectionDatabase.h>
#include <Reflection/Enum.h>
#include <Rendering/ExternalReflection.h>
#include <Rendering/Geometry/BoundingSphere.h>
#include <Rendering/Geometry/Bounds.h>
#include <ServiceLocator.h>

#include "Core/Context.h"
#include "Core/EditorActions.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetPath.h"
#include "Panels/VfxEditor.h"
#include "Panels/AssetBrowser.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Inspector/EditorSelectionService.h"
#include "Inspector/AssetTargetAdapter.h"
#include "Inspector/InspectorRegistry.h"
#include "Inspector/InspectorLayout.h"
#include "Properties/PropertyDrawerRegistry.h"
#include <UI/Controls/ScalarControl.h>
#include "Inspector/NativeReflectionAdapter.h"
#include "Inspector/ObjectReferencePicker.h"
#include "Inspector/ObjectReferenceSupport.h"
#include "Inspector/PropertyTypeTraits.h"
#include "Inspector/ProjectSettingsTargetAdapter.h"
#include "Profiling/Profiler.h"
#include "EditorProject_MetaGenerated.h"

namespace NLS::Editor::Inspector
{
namespace
{
bool EvaluateCondition(const PropertyEditorContext& context,const PropertyNode& property,std::string_view condition)
{
    if(condition.empty()) return true;
    if(!context.object) return false;
    auto cursor=context.object->FindProperty(condition);
    if(!cursor)
    {
        const auto& path=property.path.ToString();const auto dot=path.find_last_of('.');
        if(dot!=std::string::npos) cursor=context.object->FindProperty(path.substr(0,dot+1)+std::string(condition));
    }
    return cursor && cursor.GetBoolValue();
}

std::optional<Serialization::SerializedValue>& PropertyClipboard()
{
    return Properties::GetCollectionClipboard().value;
}


void EnsureDefaultInspectorAdapters()
{
    static std::once_flag once;
    std::call_once(once, []
                   {
        InspectorRegistry::Get().RegisterTargetAdapter(std::make_shared<AssetTargetAdapter>());
        InspectorRegistry::Get().RegisterTargetAdapter(std::make_shared<ProjectSettingsTargetAdapter>());
        InspectorRegistry::Get().RegisterTargetAdapter(std::make_shared<NativeReflectionAdapter>()); });
    // Generated descriptors are scanned only after reflection is live. The
    // registry suppresses a repeat scan for the same module descriptor table,
    // while changed plugin/domain descriptors still receive normal conflict
    // validation.
    NLS_META_GENERATED_LINK_FUNCTION();
    (void)meta::ReflectionDatabase::Instance();
    NLS_META_GENERATED_REGISTER_EDITOR_EXTENSIONS(InspectorRegistry::Get());
}

void CopyToBuffer(const std::string& value, std::vector<char>& buffer, const size_t minimumCapacity = 128u)
{
    buffer.assign((std::max)(minimumCapacity, value.size() + 1u), '\0');
    if (!value.empty())
        std::memcpy(buffer.data(), value.data(), value.size());
}

std::string MakeWidgetId(const PropertyPath& path, const std::string_view suffix = {})
{
    return "##Inspector." + path.ToString() + std::string(suffix);
}

void DrawTooltipIfHovered(const std::string& tooltip)
{
    if (!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip.c_str());
}

std::string TruncateUtf8ForWidth(const std::string& text, const float width)
{
    if (width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= width)
        return text;

    constexpr std::string_view suffix = "...";
    std::string result;
    result.reserve(text.size());
    for (size_t offset = 0u; offset < text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        const size_t length = (lead & 0x80u) == 0u    ? 1u :
                              (lead & 0xe0u) == 0xc0u ? 2u :
                              (lead & 0xf0u) == 0xe0u ? 3u :
                              (lead & 0xf8u) == 0xf0u ? 4u :
                                                        1u;
        if (offset + length > text.size())
            break;

        const auto candidate = result + text.substr(offset, length) + std::string(suffix);
        if (ImGui::CalcTextSize(candidate.c_str()).x > width)
            break;
        result += text.substr(offset, length);
        offset += length;
    }

    return result.empty() ? std::string(suffix) : result + std::string(suffix);
}

} // namespace

class InspectorPropertyEditor::ScopedContext
{
public:
    ScopedContext(InspectorPropertyEditor& editor, const PropertyEditorContext& context, PropertyDrawResult& result)
        : m_editor(editor), m_previous(editor.m_context), m_previousResult(editor.m_result),
          m_previousStructure(editor.m_structureChanged),m_previousInteraction(editor.m_interaction)
    {
        if (!m_previous && editor.m_drawerVersion != InspectorRegistry::Get().GetVersion())
        {
            if(editor.m_drawerVersion && context.object && context.object->HasModifiedProperties()) {context.object->DiscardModifiedProperties();result.canceled=true;}
            editor.m_controlStates.clear();
            editor.m_drawerVersion = InspectorRegistry::Get().GetVersion();
        }
        ImGui::PushID(&editor);
        std::string identity;
        if(context.object) for(const auto& target:context.object->GetTargetHandles())
            identity += target.resourcePath+"#"+target.assetId+":"+std::to_string(target.instanceId)+":"+target.typeName+"|";
        ImGui::PushID(identity.c_str());
        editor.m_context = &context;
        editor.m_result = &result;
    }
    ~ScopedContext()
    {
        ImGui::PopID();ImGui::PopID();
        m_editor.m_context = m_previous;
        m_editor.m_result = m_previousResult;
        m_editor.m_structureChanged = m_previousStructure;
        m_editor.m_interaction=m_previousInteraction;
        if(!m_editor.m_context && m_editor.m_controlStateInvalidated)
        {m_editor.m_controlStates.clear();m_editor.m_controlStateInvalidated=false;}
    }

private:
    InspectorPropertyEditor& m_editor;
    const PropertyEditorContext* m_previous;
    PropertyDrawResult* m_previousResult;
    bool m_previousStructure;
    UI::Controls::ControlResult m_previousInteraction;
};

InspectorPropertyEditor::InspectorPropertyEditor()
{
    EnsureDefaultInspectorAdapters();
    Properties::EnsureBuiltinPropertyDrawers();
    auto& registry=InspectorRegistry::Get();m_registryLifetime=registry.GetLifetime();
    m_drawerListener=registry.AddPropertyDrawerInvalidationListener([this,lifetime=std::weak_ptr<const void>(m_listenerLifetime)]
    {
        if(lifetime.expired()) return;
        ClearControlState();
    });
}

bool InspectorPropertyEditor::HasInvalidatedDrawers() const
{return m_drawerVersion!=0 && m_drawerVersion!=InspectorRegistry::Get().GetVersion();}

InspectorPropertyEditor::~InspectorPropertyEditor()
{
    m_listenerLifetime.reset();
    if(!m_registryLifetime.expired()) InspectorRegistry::Get().RemovePropertyDrawerInvalidationListener(m_drawerListener);
}

PropertyDrawResult InspectorPropertyEditor::DrawProperties(const std::vector<PropertyNode>& properties, const PropertyEditorContext& context)
{
    PropertyDrawResult result;
    ScopedContext scope(*this, context, result);
    DrawPropertiesImpl(properties);
    return result;
}

PropertyDrawResult InspectorPropertyEditor::DrawProperty(const PropertyNode& property, const PropertyEditorContext& context, PropertyEditorLayout layout)
{
    PropertyDrawResult result;
    ScopedContext scope(*this, context, result);
    if (layout == PropertyEditorLayout::Properties)
        DrawPropertiesImpl({property});
    else if (layout == PropertyEditorLayout::Compact)
        DrawPropertyCompact(property);
    else if (layout == PropertyEditorLayout::TableRow)
        DrawPropertyRow(property);
    else
        DrawPropertyValue(property);
    return result;
}

PropertyDrawResult InspectorPropertyEditor::FinishItem(const PropertyNode& property, const PropertyEditorContext& context)
{
    PropertyDrawResult result;
    ScopedContext scope(*this, context, result);
    CommitOrCancelAfterItem(property);
    return result;
}

void InspectorPropertyEditor::DrawPrefabOverrideMenu(const PropertyNode& property, const char* popupId)
{
    if (m_context->drawOverrideMenu)
        m_context->drawOverrideMenu(property, popupId);
}

Serialization::SerializedProperty InspectorPropertyEditor::ResolveSerializedProperty(const PropertyNode& property) const
{
    if (m_context->resolve)
        return m_context->resolve(property);
    return m_context->object ? m_context->object->FindProperty(property.path.ToString()) : Serialization::SerializedProperty{};
}

bool InspectorPropertyEditor::PreviewSerializedProperty(const PropertyNode& property, meta::Variant value, std::string,uint64_t components)
{
    const auto cursor = ResolveSerializedProperty(property);
    if(HasInvalidatedDrawers())
    {if(m_context->object) m_context->object->DiscardModifiedProperties();m_result->canceled=true;return false;}
    if (!cursor || !cursor.IsEditable() || property.readOnly || property.metadata.readOnly)
        return false;
    if(!EvaluateCondition(*m_context,property,property.metadata.enabledIf)) return false;
    if(!value.IsValid() || (property.valueType.IsValid() && value.GetType()!=property.valueType &&
        (!IsNumericPropertyKind(property.kind) || GetInspectorPropertyKind(value.GetType())!=property.kind)))
    {m_error="Drawer returned an incompatible property value";return false;}
    const auto minimum = property.metadata.rangeMin ? property.metadata.rangeMin : property.metadata.min;
    const auto maximum = property.metadata.rangeMax ? property.metadata.rangeMax : property.metadata.max;
    bool limitsValid=true;
    const auto clamp = [&]<class T>(T number)
    {
        T low=(std::numeric_limits<T>::lowest)(), high=(std::numeric_limits<T>::max)();
        const auto endpoint=[](float n)
        {
            if(static_cast<long double>(n)>=(std::numeric_limits<T>::max)()) return (std::numeric_limits<T>::max)();
            if(static_cast<long double>(n)<=(std::numeric_limits<T>::lowest)()) return (std::numeric_limits<T>::lowest)();
            return static_cast<T>(n);
        };
        if(minimum) low=endpoint(*minimum);if(maximum) high=endpoint(*maximum);
        if(property.metadata.numericMinimum && !UI::Controls::EvaluateExpressionAs(*property.metadata.numericMinimum,low,m_error)) limitsValid=false;
        if(property.metadata.numericMaximum && !UI::Controls::EvaluateExpressionAs(*property.metadata.numericMaximum,high,m_error)) limitsValid=false;
        if(low>high) {limitsValid=false;m_error="Invalid numeric limits";return number;}
        if constexpr(std::is_floating_point_v<T>) if(!std::isfinite(number)) {limitsValid=false;m_error="Value must be finite";return number;}
        return std::clamp(number,low,high);
    };
    if (minimum || maximum || property.metadata.numericMinimum || property.metadata.numericMaximum || IsNumericPropertyKind(property.kind))
        switch (property.kind)
        {
            case InspectorPropertyKind::Float:
                value = clamp(value.GetValue<float>());
                break;
            case InspectorPropertyKind::Double:
                value = clamp(value.GetValue<double>());
                break;
            case InspectorPropertyKind::SignedInteger8: value = clamp(value.GetValue<int8_t>()); break;
            case InspectorPropertyKind::UnsignedInteger8: value = clamp(value.GetValue<uint8_t>()); break;
            case InspectorPropertyKind::SignedInteger16: value = clamp(value.GetValue<int16_t>()); break;
            case InspectorPropertyKind::UnsignedInteger16: value = clamp(value.GetValue<uint16_t>()); break;
            case InspectorPropertyKind::SignedInteger64: value = clamp(value.GetValue<int64_t>()); break;
            case InspectorPropertyKind::UnsignedInteger64: value = clamp(value.GetValue<uint64_t>()); break;
            case InspectorPropertyKind::SignedInteger32:
                value = clamp(value.GetValue<int32_t>());
                break;
            case InspectorPropertyKind::UnsignedInteger32:
                value = clamp(value.GetValue<uint32_t>());
                break;
            case InspectorPropertyKind::Vector2:
            {
                auto v = value.GetValue<Maths::Vector2>();
                value = Maths::Vector2(clamp(v.x), clamp(v.y));
                break;
            }
            case InspectorPropertyKind::Vector3:
            {
                auto v = value.GetValue<Maths::Vector3>();
                value = Maths::Vector3(clamp(v.x), clamp(v.y), clamp(v.z));
                break;
            }
            case InspectorPropertyKind::Vector4:
            {
                auto v = value.GetValue<Maths::Vector4>();
                value = Maths::Vector4(clamp(v.x), clamp(v.y), clamp(v.z), clamp(v.w));
                break;
            }
            case InspectorPropertyKind::Matrix:
            {auto matrix=value.GetValue<Maths::Matrix4>();for(auto& number:matrix.data) number=clamp(number);value=matrix;break;}
            case InspectorPropertyKind::Color:
            {
                auto v = value.GetValue<Maths::Color>();
                value = Maths::Color(clamp(v.r), clamp(v.g), clamp(v.b), clamp(v.a));
                break;
            }
            default:
                break;
        }
    if (!limitsValid || !(components==~0ull?cursor.SetValue(value):cursor.SetValueComponents(value,components)))
        return false;
    m_error.clear();
    NotifyPropertyChanged(property);
    return true;
}

bool InspectorPropertyEditor::CommitSerializedEdit()
{
    if(HasInvalidatedDrawers())
    {if(m_context->object) m_context->object->DiscardModifiedProperties();m_result->canceled=true;return false;}
    m_result->completed = true;
    if (!m_context->object || !m_context->object->HasModifiedProperties())
        return false;
    const bool applied = m_context->apply ? m_context->apply() : m_context->object->ApplyModifiedProperties();
    m_result->applied |= applied;
    if(!applied && m_context->object->HasModifiedProperties())
    {
        m_error=m_context->object->GetLastError();
        m_context->object->DiscardModifiedProperties();
        m_result->canceled=true;m_result->completed=false;
    }
    return applied;
}

void InspectorPropertyEditor::CommitOrCancelAfterItem(const PropertyNode& property)
{
    if (ImGui::IsItemDeactivatedAfterEdit())
        m_result->completed = true;
    if (!m_context->object || !m_context->object->HasModifiedProperties())
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_context->object->DiscardModifiedProperties();
        m_result->canceled = true;
        NotifyPropertyChanged(property);
    }
    else if (ImGui::IsItemDeactivatedAfterEdit())
        CommitSerializedEdit();
}

void InspectorPropertyEditor::NotifyPropertyChanged(const PropertyNode& property)
{
    const auto path = property.path.ToString();
    if (std::find(m_result->changedPaths.begin(), m_result->changedPaths.end(), path) == m_result->changedPaths.end())
        m_result->changedPaths.push_back(path);
    if (m_context->changed)
        m_context->changed(property);
}

float InspectorPropertyEditor::GetUiScale() const
{
    if (m_context->uiScale > 0.0f)
        return m_context->uiScale;
    if (NLS::Core::ServiceLocator::Contains<Core::Context>())
    {
        const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
        if (context.uiManager)
            return (std::max)(0.75f, context.uiManager->GetScale());
    }
    return 1.0f;
}

void InspectorPropertyEditor::DrawPropertiesImpl(const std::vector<PropertyNode>& properties)
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::DrawProperties");
    if (properties.empty())
    {
        ImGui::TextDisabled("No editable properties");
        return;
    }

    const auto layout = BuildInspectorPropertiesLayout(
        ImGui::GetContentRegionAvail().x,
        GetUiScale(),
        m_context->labelColumnLogicalWidth);
    if (layout.compact)
    {
        const auto drawCompactNodes = [this](auto&& self, const std::vector<PropertyNode>& nodes) -> void
        {
            for (const auto& property : nodes)
            {
                if (!IsPropertyVisible(property))
                    continue;

                const auto drawer = Properties::PropertyDrawerRegistry::Get().Resolve(property);
                const bool isGroup = property.kind == InspectorPropertyKind::Struct && !property.children.empty() &&
                    (!drawer || drawer->scope == InspectorDrawerScope::Builtin);
                if (!isGroup)
                {
                    DrawPropertyCompact(property);
                    continue;
                }

                DrawPropertyDecorators(property);
                if (!property.metadata.header.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", property.metadata.header.c_str());
                }
                if (property.metadata.spaceBefore > 0.0f)
                    ImGui::Dummy(ImVec2(0.0f, property.metadata.spaceBefore * GetUiScale()));

                ImGui::PushID(property.path.ToString().c_str());
                const bool opened = ImGui::TreeNodeEx(
                    TruncateUtf8ForWidth(property.displayName, ImGui::GetContentRegionAvail().x).c_str(),
                    ImGuiTreeNodeFlags_SpanAvailWidth);
                DrawTooltipIfHovered(property.metadata.tooltip);
                DrawPrefabOverrideMenu(property, "##PrefabPropertyMenu");
                if (opened)
                {
                    self(self, property.children);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        };
        drawCompactNodes(drawCompactNodes, properties);
        return;
    }

    const float scale = GetUiScale();

    if (!ImGui::BeginTable("##InspectorProperties", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable))
    {
        return;
    }

    ImGui::TableSetupColumn("##InspectorLabel", ImGuiTableColumnFlags_WidthFixed, layout.labelLogicalWidth * scale);
    ImGui::TableSetupColumn("##InspectorValue", ImGuiTableColumnFlags_WidthStretch);
    const auto drawRows = [this](auto&& self, const std::vector<PropertyNode>& nodes) -> void
    {
        for (const auto& property : nodes)
        {
            if (!IsPropertyVisible(property))
                continue;

            const auto drawer = Properties::PropertyDrawerRegistry::Get().Resolve(property);
                const bool isGroup = property.kind == InspectorPropertyKind::Struct && !property.children.empty() &&
                    (!drawer || drawer->scope == InspectorDrawerScope::Builtin);
            if (!isGroup)
            {
                DrawPropertyRow(property);
                continue;
            }

            DrawPropertyDecorators(property);
            if (!property.metadata.header.empty())
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", property.metadata.header.c_str());
            }
            if (property.metadata.spaceBefore > 0.0f)
                ImGui::TableNextRow(ImGuiTableRowFlags_None, property.metadata.spaceBefore * GetUiScale());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(property.path.ToString().c_str());
            const bool opened = ImGui::TreeNodeEx(
                TruncateUtf8ForWidth(property.displayName, ImGui::GetContentRegionAvail().x).c_str(),
                ImGuiTreeNodeFlags_SpanAvailWidth);
            DrawTooltipIfHovered(property.metadata.tooltip);
            DrawPrefabOverrideMenu(property, "##PrefabPropertyMenu");
            ImGui::TableSetColumnIndex(1);
            if (!property.diagnostic.empty())
                ImGui::TextDisabled("%s", property.diagnostic.c_str());
            if (opened)
            {
                self(self, property.children);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    };
    drawRows(drawRows, properties);
    ImGui::EndTable();
}

void InspectorPropertyEditor::DrawPropertyCompact(const PropertyNode& property)
{
    DrawPropertyDecorators(property);
    if (!property.metadata.header.empty())
    {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", property.metadata.header.c_str());
    }
    if (property.metadata.spaceBefore > 0.0f)
        ImGui::Dummy(ImVec2(0.0f, property.metadata.spaceBefore * GetUiScale()));

    if(property.kind != InspectorPropertyKind::Array && property.kind != InspectorPropertyKind::Struct)
        DrawPropertyLabel(property, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(-FLT_MIN);
    DrawPropertyValue(property);
    ImGui::Spacing();
}

void InspectorPropertyEditor::DrawPropertyRow(const PropertyNode& property)
{
    DrawPropertyDecorators(property);
    if (!property.metadata.header.empty())
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", property.metadata.header.c_str());
    }
    if (property.metadata.spaceBefore > 0.0f)
    {
        ImGui::TableNextRow(ImGuiTableRowFlags_None, property.metadata.spaceBefore * GetUiScale());
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if(property.kind == InspectorPropertyKind::Array || property.kind == InspectorPropertyKind::Struct)
    {
        // A list owns its label, size field and boxed body across both columns.
        auto* window = ImGui::GetCurrentWindow();
        auto* table = GImGui->CurrentTable;
        const auto work = window->WorkRect;
        const auto content = window->ContentRegionRect;
        const float cursorMax = window->DC.CursorMaxPos.x;
        const float idealMax = window->DC.IdealMaxPos.x;
        window->WorkRect.Max.x = window->ContentRegionRect.Max.x = table->WorkRect.Max.x;
        ImGui::PushClipRect({table->InnerClipRect.Min.x, window->ClipRect.Min.y},
                            {table->InnerClipRect.Max.x, window->ClipRect.Max.y}, false);
        DrawPropertyValue(property);
        ImGui::PopClipRect();
        window->WorkRect = work;
        window->ContentRegionRect = content;
        window->DC.CursorMaxPos.x = cursorMax;
        window->DC.IdealMaxPos.x = idealMax;
        return;
    }
    DrawPropertyLabel(property, ImGui::GetContentRegionAvail().x);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    DrawPropertyValue(property);
}

void InspectorPropertyEditor::DrawPropertyDecorators(const PropertyNode& property)
{
    for (const auto& registration : InspectorRegistry::Get().ResolveDecoratorDrawers(property))
    {
        if (registration.draw)
            registration.draw({property, GetUiScale()});
    }
}

void InspectorPropertyEditor::DrawPropertyLabel(const PropertyNode& property, const float availableWidth)
{
    const std::string label = TruncateUtf8ForWidth(property.displayName, availableWidth);
    if (property.prefabOverride != PrefabOverrideState::None)
        ImGui::TextColored(ImVec4(0.36f, 0.70f, 1.0f, 1.0f), "%s", label.c_str());
    else
        ImGui::TextUnformatted(label.c_str());

    std::string tooltip = property.metadata.tooltip;
    if (tooltip.empty() && label != property.displayName)
        tooltip = property.displayName;
    if (!property.diagnostic.empty())
    {
        if (!tooltip.empty())
            tooltip += "\n";
        tooltip += property.diagnostic;
    }
    DrawTooltipIfHovered(tooltip);

    DrawPrefabOverrideMenu(property, "##PrefabPropertyMenu");
    if(property.prefabOverride==PrefabOverrideState::None) DrawPropertyMenu(property);
}

bool InspectorPropertyEditor::DrawPropertyValue(const PropertyNode& property)
{
    const auto cursor = ResolveSerializedProperty(property);
    const auto registration=Properties::PropertyDrawerRegistry::Get().Resolve(property);
    const bool lazyComposite=(property.kind==InspectorPropertyKind::Array || property.kind==InspectorPropertyKind::Struct) && registration && registration->scope==InspectorDrawerScope::Builtin;
    auto value = cursor && !lazyComposite ? cursor.GetValue() : meta::Variant{};
    if (!value.IsValid() && !lazyComposite)
    {
        ImGui::TextDisabled("%s", !property.diagnostic.empty() ? property.diagnostic.c_str() : "Unavailable");
        return false;
    }
    if (cursor.GetValueState() == PropertyValueState::Invalid)
    { ImGui::TextDisabled("Incompatible selection"); return false; }
    ImGui::PushID(property.path.ToString().c_str());
    const bool enabled=EvaluateCondition(*m_context,property,property.metadata.enabledIf);
    const bool readOnly = property.readOnly || property.metadata.readOnly || !cursor.IsEditable() || !enabled;
    ImGui::BeginDisabled(readOnly && property.kind != InspectorPropertyKind::Array);
    if(m_drawFrame!=ImGui::GetFrameCount()) {m_drawFrame=ImGui::GetFrameCount();m_frameActive=false;}
    m_interaction = {};
    m_structureChanged = false;
    if(cursor.HasMultipleDifferentValues() && property.kind==InspectorPropertyKind::ObjectReference)
        UI::Controls::MixedLabel({false,true,GetUiScale()});
    bool completed = false;
    const bool changed = DrawRegisteredValue(property,value,MakeWidgetId(property.path),completed);
    ImGui::EndDisabled();
    if(!enabled || property.readOnly || property.metadata.readOnly || !cursor.IsEditable()) {m_interaction={};completed=false;}
    if (m_interaction.canceled)
    {
        m_context->object->DiscardModifiedProperties();
        m_result->canceled = true;
        NotifyPropertyChanged(property);
    }
    else if (!m_interaction.active && completed)
        CommitSerializedEdit();
    else if (!m_interaction.active)
        CommitOrCancelAfterItem(property);
    if(property.kind!=InspectorPropertyKind::Array) DrawPropertyMenu(property);
    if (m_structureChanged && m_context->structureChanged) m_context->structureChanged();
    ImGui::PopID();
    return changed;
}

bool InspectorPropertyEditor::DrawRegisteredValue(const PropertyNode& property, meta::Variant& value,
    const std::string& id,bool& completed)
{
    const auto registration = Properties::PropertyDrawerRegistry::Get().Resolve(property);
    if (!registration || !registration->draw) {ImGui::TextDisabled("Read only: %s",property.valueType.GetName().c_str());return false;}
    InspectorPropertyDrawerContext context{property,value,id,GetUiScale()};
    auto cursor = ResolveSerializedProperty(property);
    context.serializedProperty = cursor ? &cursor : nullptr;
    context.objectReferencePickerState = &m_objectReferencePickerState;
    auto& controlState=m_controlStates[ImGui::GetID(id.c_str())];
    const auto structureVersion=m_context->object?m_context->object->GetVersion().structureGeneration:0;
    if(property.kind!=InspectorPropertyKind::Array && controlState.structureVersion!=structureVersion)
        controlState=Properties::PropertyControlState{};
    controlState.structureVersion=structureVersion;
    context.state = &controlState;
    context.labelColumnWidth=GImGui->CurrentTable?GImGui->CurrentTable->Columns[0].WidthGiven:
        BuildInspectorPropertiesLayout(ImGui::GetContentRegionAvail().x,GetUiScale(),m_context->labelColumnLogicalWidth).labelLogicalWidth*GetUiScale();
    context.preview = [&](meta::Variant updated,std::string label)
    {
        if (cursor)
        {
            bool applied = false;
            // Stage component leaves independently, preserving every unedited
            // component of secondary targets in a mixed selection.
            const auto mask = context.interaction.components;
            const bool components = property.kind == InspectorPropertyKind::Vector2 || property.kind == InspectorPropertyKind::Vector3 ||
                property.kind == InspectorPropertyKind::Vector4 || property.kind == InspectorPropertyKind::Color || property.kind == InspectorPropertyKind::Quaternion ||
                property.kind == InspectorPropertyKind::Rect || property.kind == InspectorPropertyKind::Vector2Int || property.kind == InspectorPropertyKind::Vector3Int ||
                property.kind == InspectorPropertyKind::RectInt || property.kind == InspectorPropertyKind::Bounds || property.kind == InspectorPropertyKind::BoundsInt ||
                property.kind == InspectorPropertyKind::BoundingSphere || property.kind == InspectorPropertyKind::Matrix;
            applied=PreviewSerializedProperty(property,updated,std::move(label),components && mask?mask:~0ull);
            context.changed |= applied;
            if (applied) value = std::move(updated);
            return applied;
        }
        value = std::move(updated);context.changed = true;return true;
    };
    context.commit = [&] {return cursor && cursor.IsEditable() && !property.readOnly && !property.metadata.readOnly && EvaluateCondition(*m_context,property,property.metadata.enabledIf)?CommitSerializedEdit():false;};
    context.drawReference = [&]
    {bool done=false;const bool changed=DrawObjectReferenceValue(property,value,id,done);return UI::Controls::ControlResult{changed,false,changed,done,false,changed?~0ull:0};};
    context.drawStructure = [&] {return cursor?DrawSerializedStruct(property):UI::Controls::ControlResult{};};
    context.serializedObject = m_context->object;
    context.readOnly = property.readOnly || property.metadata.readOnly || !cursor.IsEditable() || !EvaluateCondition(*m_context,property,property.metadata.enabledIf);
    context.notifyChanged = [&](const PropertyNode& node) {NotifyPropertyChanged(node);};
    context.drawMenu = [&] {DrawPropertyMenuItems(property,false);};
    context.setExpanded = [&](std::string_view path,bool expanded) {m_expansionOverrides[std::string(path)]=expanded;};
    if(auto found=m_expansionOverrides.find(property.path.ToString());found!=m_expansionOverrides.end())
    {context.expandedOverride=found->second;if(property.kind==InspectorPropertyKind::Array) m_expansionOverrides.erase(found);}
    context.drawChild = [&](const PropertyNode& node)
    {
        const auto outer = m_interaction;
        const bool outerStructure = m_structureChanged;
        const bool changed = DrawPropertyValue(node);
        auto result = m_interaction;
        result.changed |= changed;
        m_interaction = outer;
        m_structureChanged |= outerStructure;
        return result;
    };
    context.drawCollection = [&] {Properties::DrawCollectionProperty(context);return context.interaction;};
    if (!registration->draw(context))
    {
        const auto builtin=InspectorRegistry::Get().ResolveBuiltinPropertyDrawer(property);
        if(!builtin || builtin->identifier==registration->identifier || !builtin->draw(context))
        {ImGui::TextDisabled("Read only");return false;}
    }
    m_frameActive |= context.interaction.active;
    m_interaction.Merge(context.interaction);
    completed |= context.interaction.completed;
    m_structureChanged |= context.structureChanged;
    return context.changed;
}

bool InspectorPropertyEditor::DrawObjectReferenceValue(
    const PropertyNode& property,
    meta::Variant& value,
    const std::string& id, bool& editCompleted)
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::DrawObjectReferenceValue");
    const std::string display = GetInspectorObjectReferenceDisplay(value, property.valueType);
    const auto commit = [&value, &editCompleted](meta::Variant updated, const char*)
    { value = std::move(updated); editCompleted = true; return true; };

    const auto referenceType = property.valueType;
    InspectorObjectReferencePickerConfig picker;
    picker.id = id;
    picker.searchKey = property.path.ToString();
    picker.displayName = display;
    picker.displayTooltip = display;
    picker.noMatchesLabel = "No compatible references";
    picker.uiScale = GetUiScale();
    if (IsInspectorAssetObjectReferenceType(referenceType))
    {
        picker.acceptsAsset = [referenceType](const Assets::EditorAssetDragPayload& payload)
        {
            return IsCompatibleInspectorReferenceArtifact(
                referenceType,
                Assets::GetEditorAssetDragPayloadArtifactType(payload));
        };
        picker.assignAsset = [&value, referenceType, commit](const Assets::EditorAssetDragPayload& payload)
        {
            auto updated = value;
            return AssignInspectorAssetReference(updated, referenceType, payload) && commit(std::move(updated), "Assign Object Reference");
        };
    }
    if (const auto pingTarget = GetInspectorObjectReferencePingTarget(value, referenceType); pingTarget.has_value())
    {
        picker.ping = [target = *pingTarget]
        {
            EditorSelectionService::Get().Select(target);
        };
        if (IsCompatibleInspectorReferenceArtifact(referenceType, NLS::Core::Assets::ArtifactType::VfxSystem) && NLS::Core::ServiceLocator::Contains<Core::EditorActions>())
        {
            picker.open = [target = *pingTarget]
            {
                const auto roots = Assets::MakeProjectEditorAssetRoots(EDITOR_CONTEXT(projectPath));
                Assets::AssetDatabaseFacade database(roots);
                auto path = target.resourcePath;
                if (!target.assetId.empty() && database.Refresh())
                {
                    const auto current = database.GUIDToAssetPath(target.assetId);
                    if (!current.empty())
                        path = current;
                }
                if (const auto separator = path.find('#'); separator != std::string::npos)
                    path.erase(separator);
                const auto source = Assets::ResolveEditorAssetPath(roots, path);
                if (!source.empty())
                    (void)EDITOR_PANEL(Panels::VfxEditor, "Particle Editor").OpenDocument(source);
            };
        }
    }
    picker.clear = [&value, referenceType, commit]
    {
        auto updated = value;
        return ClearInspectorObjectReference(updated, referenceType) && commit(std::move(updated), "Clear Object Reference");
    };
    picker.buildItems = [this, &property, &value, referenceType, commit]
    {
        std::vector<InspectorObjectReferencePickerItem> items;
        if (IsInspectorAssetObjectReferenceType(referenceType))
        {
            // Opening the picker must never present a stale or empty catalog.
            // The shared entry cache only rebuilds while it is marked dirty, so
            // flush a pending refresh before enumerating the candidates.
            static_cast<void>(Assets::RefreshObjectReferencePickerEntries());
            for (const auto& entry : Assets::GetObjectReferencePickerEntries())
            {
                if (!IsCompatibleInspectorReferenceArtifact(
                        referenceType,
                        Assets::GetEditorAssetDragPayloadArtifactType(entry.payload)))
                {
                    continue;
                }
                const auto payload = entry.payload;
                items.push_back({entry.displayName,
                                 entry.displayName,
                                 [&value, referenceType, payload, commit]
                                 {
                                     auto updated = value;
                                     return AssignInspectorAssetReference(updated, referenceType, payload) && commit(std::move(updated), "Assign Object Reference");
                                 }});
            }
        }

        if (IsInspectorSceneObjectReferenceType(referenceType))
        {
            auto* ownerObject = NativeReflectionAdapter::ResolveObject(property.target);
            auto* ownerGameObject = dynamic_cast<Engine::GameObject*>(ownerObject);
            if (auto* ownerComponent = dynamic_cast<Engine::Components::Component*>(ownerObject))
                ownerGameObject = ownerComponent->gameobject();
            const auto* scene = ownerGameObject != nullptr ? ownerGameObject->GetScene() : nullptr;
            for (const auto& entry : BuildInspectorSceneObjectReferencePickerEntries(referenceType, scene))
            {
                auto* gameObject = entry.gameObject;
                auto* component = entry.component;
                items.push_back({entry.displayName,
                                 entry.displayName,
                                 [&value, referenceType, gameObject, component, commit]
                                 {
                                     auto updated = value;
                                     return AssignInspectorSceneReference(
                                                updated,
                                                referenceType,
                                                gameObject,
                                                component)
                                            && commit(std::move(updated), "Assign Object Reference");
                                 }});
            }
        }
        return items;
    };
    const bool changed=DrawInspectorObjectReferencePicker(m_objectReferencePickerState,picker);
    if(IsInspectorAssetObjectReferenceType(referenceType) && NLS::Core::ServiceLocator::Contains<Core::EditorActions>())
    {
        const auto target=GetInspectorObjectReferencePingTarget(value,referenceType);
        if(target)
        {
            auto& state=m_controlStates[ImGui::GetID(id.c_str())];
            UI::Controls::PreviewRequest request;request.identity=target->assetId+":"+target->resourcePath;
            request.revision=m_context->object?m_context->object->GetVersion().dataGeneration:0;request.lifetime=state.lifetime;
            UI::Controls::ResourcePreview((id+".preview").c_str(),state.preview,request,[target](const auto& query)
            {return EDITOR_PANEL(Panels::AssetBrowser,"Asset Browser").RequestPropertyPreview(target->assetId,target->resourcePath,query);});
        }
    }
    return changed;
}

bool InspectorPropertyEditor::IsPropertyVisible(const PropertyNode& property) const
{
    if(property.metadata.hideInInspector) return false;
    return EvaluateCondition(*m_context,property,property.metadata.visibleIf);
}

void InspectorPropertyEditor::DrawPropertyMenu(const PropertyNode& property)
{
    const std::string popup="##PropertyMenu."+property.path.ToString()+":"+std::to_string(ImGui::GetItemID());
    if(!ImGui::BeginPopupContextItem(popup.c_str())) return;
    DrawPropertyMenuItems(property,true);
    ImGui::EndPopup();
}

void InspectorPropertyEditor::DrawPropertyMenuItems(const PropertyNode& property, bool clipboardItems)
{
    auto& clipboard=PropertyClipboard();
    const auto cursor=ResolveSerializedProperty(property);
    if(!cursor) return;
    const bool editable=cursor.IsEditable() && !property.readOnly && !property.metadata.readOnly && EvaluateCondition(*m_context,property,property.metadata.enabledIf);
    if(clipboardItems)
    {
        if(ImGui::MenuItem("Copy value")) {clipboard=Serialization::SerializedValue::Capture(cursor.GetValue());Properties::GetCollectionClipboard().elements.clear();}
        const auto copied=clipboard?clipboard->Resolve(property.valueType):std::nullopt;
        if(ImGui::MenuItem("Paste value",nullptr,false,editable && copied.has_value()))
        {if(PreviewSerializedProperty(property,*copied,"Paste property")) CommitSerializedEdit();}
    }
    const auto defaults=m_context->defaultValue?m_context->defaultValue(property):std::nullopt;
    if(ImGui::MenuItem("Restore default",nullptr,false,editable && defaults.has_value()))
    {if(PreviewSerializedProperty(property,*defaults,"Restore default")) CommitSerializedEdit();}
    const auto inherited=m_context->inheritedValue?m_context->inheritedValue(property):std::nullopt;
    if((inherited || m_context->restoreInherited) && ImGui::MenuItem("Restore inherited value",nullptr,false,editable))
    {
        if(m_context->restoreInherited)
        {
            if(m_context->restoreInherited(property))
            {m_context->object->DiscardModifiedProperties();m_result->hostAction=m_result->applied=m_result->completed=true;}
            else m_error="The host could not restore the inherited value";
        }
        else if(PreviewSerializedProperty(property,*inherited,"Restore inherited value")) CommitSerializedEdit();
    }
    if(m_context->drawOverrideMenuItems) m_context->drawOverrideMenuItems(property);
}

UI::Controls::ControlResult InspectorPropertyEditor::DrawSerializedStruct(const PropertyNode& property)
{
    UI::Controls::ControlResult result;
    auto cursor=ResolveSerializedProperty(property);
    if(!cursor) return result;
    const auto expansion=m_expansionOverrides.find(property.path.ToString());
    if(expansion!=m_expansionOverrides.end())
    {ImGui::SetNextItemOpen(expansion->second);m_expansionOverrides.erase(expansion);}
    else if(cursor.IsExpanded()) ImGui::SetNextItemOpen(true,ImGuiCond_Once);
    if(!ImGui::TreeNodeEx("##Structure",ImGuiTreeNodeFlags_SpanAvailWidth,"%s",property.displayName.c_str())) return result;
    auto iterator=cursor.Copy();const auto depth=iterator.GetDepth();bool enter=true;
    std::vector<PropertyNode> nodes;
    while(iterator.NextVisible(enter) && iterator.GetDepth()>depth)
    {
        enter=false;
        PropertyNode node;node.path=PropertyPath(std::string(iterator.GetPropertyPath()));node.fieldName=std::string(iterator.GetName());
        node.displayName=std::string(iterator.GetDisplayName());node.metadata=iterator.GetMetadata();node.ownerTypeName=property.valueType.GetName();
        const auto* stream=m_context->object->GetPrimaryByteStream();
        const auto* schemaNode=stream && stream->GetSchema()?stream->GetSchema()->FindPath(node.path.ToString()):nullptr;
        if(schemaNode && schemaNode->typeId!=meta::InvalidTypeID) node.valueType=meta::Type(schemaNode->typeId,iterator.IsArray());
        if(!node.valueType.IsValid()) {const auto value=iterator.GetValue();node.valueType=value.GetType();}
        node.kind=iterator.IsArray()?InspectorPropertyKind::Array:GetInspectorPropertyKind(node.valueType);
        if(const auto field=property.valueType.GetField(node.fieldName);field.IsValid())
        {
            node.ownerTypeName=field.GetClassType().GetName();
            node.metadata=BuildEditorPropertyMetadata(field);
        }
        else node.metadata=BuildEditorPropertyMetadata(node.ownerTypeName,node.fieldName,node.metadata);
        if(!node.metadata.displayName.empty()) node.displayName=node.metadata.displayName;
        if(node.metadata.flags && node.kind==InspectorPropertyKind::Enum) node.kind=InspectorPropertyKind::Flags;
        node.readOnly=property.readOnly || iterator.IsReadOnly();node.targets=property.targets;node.target=property.target;
        if(IsPropertyVisible(node)) nodes.push_back(std::move(node));
    }
    const auto outer=m_interaction;const auto outerStructure=m_structureChanged;
    const auto layout=BuildInspectorPropertiesLayout(ImGui::GetContentRegionAvail().x,GetUiScale(),m_context->labelColumnLogicalWidth);
    const bool table=!layout.compact && ImGui::BeginTable("##NestedProperties",2,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_Resizable);
    if(table)
    {
        ImGui::TableSetupColumn("##Label",ImGuiTableColumnFlags_WidthFixed,layout.labelLogicalWidth*GetUiScale());
        ImGui::TableSetupColumn("##Value",ImGuiTableColumnFlags_WidthStretch);
    }
    for(const auto& node:nodes)
    {
        if(table) DrawPropertyRow(node);else DrawPropertyCompact(node);
        result.Merge(m_interaction);
        if(m_structureChanged && !outerStructure) break;
    }
    if(table) ImGui::EndTable();
    m_interaction=outer;m_structureChanged|=outerStructure;
    ImGui::TreePop();UI::Controls::FinishComposite(result);return result;
}

} // namespace NLS::Editor::Inspector
