#include "Inspector/InspectorView.h"

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

#include <Components/Component.h>
#include <Components/TransformComponent.h>
#include <Components/UICanvasComponent.h>
#include <Debug/Logger.h>
#include <GameObject.h>
#include <LayerMask.h>
#include <Math/ExternalReflection.h>
#include <Math/Quaternion.h>
#include <Math/Vector2.h>
#include <Math/Vector3.h>
#include <Math/Vector4.h>
#include <Reflection/ArrayWrapper.h>
#include <Reflection/ReflectionDatabase.h>
#include <Reflection/Enum.h>
#include <Rendering/ExternalReflection.h>
#include <Rendering/Geometry/BoundingSphere.h>
#include <Rendering/Geometry/Bounds.h>
#include <Scripting/ScriptComponent.h>
#include <ServiceLocator.h>

#include "Core/Context.h"
#include "Core/EditorActions.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetPath.h"
#include "Panels/VfxEditor.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Inspector/EditorSelectionService.h"
#include "Inspector/AssetTargetAdapter.h"
#include "Inspector/InspectorRegistry.h"
#include "Inspector/InspectorLayout.h"
#include "Inspector/InspectorProjectionBuilder.h"
#include "Inspector/NativeReflectionAdapter.h"
#include "Inspector/ObjectReferencePicker.h"
#include "Inspector/ObjectReferenceSupport.h"
#include "Inspector/PrefabInspectorSupport.h"
#include "Inspector/PropertyTypeTraits.h"
#include "Inspector/ProjectSettingsTargetAdapter.h"
#include "Inspector/ScriptSerializedAdapter.h"
#include "Serialization/AccessorSerializedBackend.h"
#include "Serialization/ImporterDraftSerializedBackend.h"
#include "Serialization/MaterialSerializedBackend.h"
#include "Serialization/NativeReflectionSerializedBackend.h"
#include "Serialization/ProjectSettingsSerializedBackend.h"
#include "Panels/ComponentSearchPanel.h"
#include "Profiling/Profiler.h"
#include "Settings/TagLayerSettings.h"
#include "Shortcuts/EditorShortcutService.h"
#include "EditorProject_MetaGenerated.h"

namespace NLS::Editor::Inspector
{
namespace
{
void CopyToBuffer(const std::string& value, std::vector<char>& buffer, const size_t minimumCapacity = 128u)
{
    buffer.assign((std::max)(minimumCapacity, value.size() + 1u), '\0');
    if (!value.empty())
        std::memcpy(buffer.data(), value.data(), value.size());
}

const PropertyNode* FindUiPropertyNode(
    const std::vector<PropertyNode>& nodes,
    const PropertyPath& path)
{
    for (const auto& node : nodes)
    {
        if (node.path == path)
            return &node;
        if (const auto* child = FindUiPropertyNode(node.children, path))
            return child;
    }
    return nullptr;
}

const PropertyNode* FindUiPropertyNode(
    const Inspector::InspectorPresentation& tree,
    const PropertyPath& path)
{
    if (const auto* node = FindUiPropertyNode(tree.roots, path))
        return node;
    return FindUiPropertyNode(tree.auxiliary, path);
}

const PropertyNode* FindComponentPropertyNode(
    const std::vector<PropertyNode>& nodes,
    const std::string_view serializedPath)
{
    for (const auto& node : nodes)
    {
        const auto nodePath = node.path.ToString();
        if (nodePath == serializedPath ||
            (nodePath.size() > serializedPath.size() &&
                nodePath.compare(nodePath.size() - serializedPath.size(), serializedPath.size(), serializedPath) == 0u &&
                nodePath[nodePath.size() - serializedPath.size() - 1u] == '.'))
        {
            return &node;
        }
        if (const auto* child = FindComponentPropertyNode(node.children, serializedPath))
            return child;
    }
    return nullptr;
}

std::string MakeComponentEditorKey(
    const PropertyNode& componentNode,
    const ComponentEditorRegistration& registration,
    const std::vector<InspectorTargetHandle>& targets)
{
    std::string key = componentNode.path.ToString();
    key += "|";
    key += registration.identifier;
    for (const auto& target : targets)
    {
        key += "|";
        key += target.typeName;
        key += ":";
        key += target.resourcePath;
        key += ":";
        key += target.assetId;
        key += ":";
        key += std::to_string(target.objectIdentifier.localIdentifierInFile);
        key += ":";
        key += target.objectIdentifier.guid.ToString();
        key += ":";
        key += std::to_string(static_cast<int>(target.kind));
    }
    return key;
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
        const size_t length = (lead & 0x80u) == 0u ? 1u :
            (lead & 0xe0u) == 0xc0u ? 2u :
            (lead & 0xf0u) == 0xe0u ? 3u :
            (lead & 0xf8u) == 0xf0u ? 4u : 1u;
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

bool ExecuteComponentStructureEdit(
    const std::string& label,
    const InspectorTargetHandle& ownerHandle,
    ComponentStructureSnapshot before,
    ComponentStructureEditOperation operation)
{
    if (!NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>())
        return false;

    auto info = BuildInspectorUndoCommandInfo(
        label,
        {ownerHandle},
        PropertyPath("GameObject.components"));
    info.estimatedBytes = sizeof(ComponentStructureSnapshot) +
        before.components.size() * sizeof(ComponentSnapshot);
    auto* owner = dynamic_cast<Engine::GameObject*>(NativeReflectionAdapter::ResolveObject(ownerHandle));
    const auto ownerTarget = owner != nullptr
        ? CaptureComponentStructureOwner(*owner, info)
        : std::nullopt;
    if (!ownerTarget.has_value())
        return false;

    auto& undo = NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>();
    return NLS::Editor::Inspector::ExecuteComponentStructureEdit(
               undo,
               std::move(info),
               *ownerTarget,
               std::move(before),
               std::move(operation))
        .Succeeded();
}

} // namespace

InspectorView::InspectorView(const bool followSelection)
{
    lineBreak = true;
    m_componentAddedListener = m_componentPicker.ComponentAddedEvent += [this]
    {
        Refresh();
        StructureChangedEvent.Invoke();
    };
    if (!followSelection) { m_locked = true; m_toolbarVisible = false; return; }
    m_selectionListener = EditorSelectionService::Get().SelectionChangedEvent += [this](const InspectorTargetSet& targets)
    {
        if (!m_locked)
            SetTargets(targets);
    };
    SetTargets(EditorSelectionService::Get().GetSelection());
}

InspectorView::~InspectorView()
{
    ClearComponentEditors();
    m_componentPicker.ComponentAddedEvent -= m_componentAddedListener;
    if (m_selectionListener != InvalidListenerID)
        EditorSelectionService::Get().SelectionChangedEvent -= m_selectionListener;
}

InspectorView::PropertyDrawResult InspectorView::DrawSerializedProperties(
    Serialization::SerializedObject& object, std::vector<PropertyNode> properties)
{
    auto context = MakePropertyEditorContext(&object);
    context.apply = [&object]
    { return object.ApplyModifiedPropertiesWithoutUndo(); };
    return m_propertyEditor.DrawProperties(properties, context);
}

PropertyEditorContext InspectorView::MakePropertyEditorContext(Serialization::SerializedObject* object)
{
    PropertyEditorContext context;
    context.object = object ? object : (m_serializedDrawOverride ? m_serializedDrawOverride->owner : m_serializedObject.get());
    if (!object)
        context.resolve = [this](const PropertyNode& property)
        { return ResolveSerializedProperty(property); };
    context.changed = [this](const PropertyNode& property)
    { NotifyPropertyChanged(property); };
    context.structureChanged = [this]
    { m_treeDirty = true; StructureChangedEvent.Invoke(); };
    context.drawOverrideMenu = [this](const PropertyNode& property, const char* popup)
    { DrawPrefabOverrideMenu(property, popup); };
    context.drawOverrideMenuItems = [this](const PropertyNode& property) {DrawPrefabOverrideMenuItems(property);};
    context.uiScale = GetUiScale();
    context.labelColumnLogicalWidth = m_labelColumnLogicalWidth;
    return context;
}

void InspectorView::DrawProperties(const std::vector<PropertyNode>& properties)
{
    (void)m_propertyEditor.DrawProperties(properties, MakePropertyEditorContext());
}
void InspectorView::DrawPropertyCompact(const PropertyNode& property)
{
    (void)m_propertyEditor.DrawProperty(property, MakePropertyEditorContext(), PropertyEditorLayout::Compact);
}
void InspectorView::DrawPropertyRow(const PropertyNode& property)
{
    (void)m_propertyEditor.DrawProperty(property, MakePropertyEditorContext(), PropertyEditorLayout::TableRow);
}

void InspectorView::SetTargets(InspectorTargetSet targets)
{
    targets.Normalize();
    if(m_drawingProperties)
    {
        if(m_serializedObject) m_serializedObject->DiscardModifiedProperties();
        if(m_serializedDrawOverride && m_serializedDrawOverride->owner) m_serializedDrawOverride->owner->DiscardModifiedProperties();
        m_propertyEditor.CancelEdits();
        m_pendingTargets=std::move(targets);
        return;
    }
    if (targets == m_targets)
        return;

    if (m_serializedObject != nullptr && m_serializedObject->HasModifiedProperties())
        m_serializedObject->DiscardModifiedProperties();
    m_propertyEditor.CancelEdits();
    ClearComponentEditors();
    m_targets = std::move(targets);
    m_treeDirty = true;
    m_prefabOverridesDirty = true;
    m_prefabActions.clear();
    m_prefabActionsResolved = false;
    m_prefabActionError.clear();
    m_componentPicker.ClearTarget();
}

const InspectorTargetSet& InspectorView::GetTargets() const
{
    return m_targets;
}

void InspectorView::Refresh()
{
    m_treeDirty = true;
    m_prefabOverridesDirty = true;
    if (m_prefabOverrideQuery != nullptr)
        m_prefabOverrideQuery->InvalidateAll();
}

void InspectorView::SetSearchText(std::string searchText)
{
    if (m_searchText == searchText)
        return;

    m_searchText = std::move(searchText);
    m_treeDirty = true;
}

void InspectorView::SetToolbarVisible(const bool visible)
{
    m_toolbarVisible = visible;
}

void InspectorView::SetLocked(const bool locked)
{
    if (m_locked == locked)
        return;

    m_locked = locked;
    if (!m_locked)
        SetTargets(EditorSelectionService::Get().GetSelection());
}

bool InspectorView::IsLocked() const
{
    return m_locked;
}

Engine::GameObject* InspectorView::GetPrimaryGameObject() const
{
    const auto* target = m_targets.Primary();
    return target != nullptr ? dynamic_cast<Engine::GameObject*>(NativeReflectionAdapter::ResolveObject(*target)) : nullptr;
}

void InspectorView::_Draw_Impl()
{
    if(m_pendingTargets)
    {auto pending=std::move(*m_pendingTargets);m_pendingTargets.reset();SetTargets(std::move(pending));}
    m_drawingProperties=true;
    struct DrawGuard {bool& drawing;~DrawGuard(){drawing=false;}} guard{m_drawingProperties};
    EnsureTree();
    RefreshPrefabOverridesIfNeeded();
    m_liveComponentEditorKeys.clear();
    if (m_toolbarVisible)
        DrawToolbar();

    if (!m_prefabActionError.empty())
        ImGui::TextColored(ImVec4(0.90f, 0.38f, 0.34f, 1.0f), "%s", m_prefabActionError.c_str());

    if (m_tree.roots.empty())
    {
        DrawMissingTarget();
        DrawTargetActions();
        PruneComponentEditors();
        return;
    }

    for (const auto& root : m_tree.roots)
    {
        if (root.target.kind == InspectorTargetKind::GameObject)
            DrawGameObject(root);
        else
            DrawProperties(root.children);
    }

    DrawTargetActions();
    PruneComponentEditors();
}

void InspectorView::EnsureTree()
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::EnsureTree");
    const auto& registry = InspectorRegistry::Get();
    const auto* adapter = registry.ResolveTargetAdapter(m_targets);
    const uint64_t structureVersion = adapter != nullptr ? adapter->GetStructureVersion(m_targets) : 0u;
    const uint64_t registryVersion = registry.GetVersion();
    const uint64_t metadataVersion = InspectorMetadataRegistry::Get().GetVersion();
    if (!m_treeDirty &&
        structureVersion == m_treeStructureVersion &&
        registryVersion == m_registryVersion &&
        metadataVersion == m_metadataVersion)
    {
        return;
    }

    // BuildPresentation only builds chrome. Prefab override markers are
    // refreshed after the projected field tree is attached below.
    m_prefabOverridesDirty = true;

    m_tree = adapter != nullptr
        ? adapter->BuildPresentation(m_targets, {m_searchText, false})
        : InspectorPresentation{};
    const bool useAccessorStream = Serialization::AccessorSerializedBackend::Supports(m_targets);
    if (m_tree.roots.empty())
    {
        m_serializedObject.reset();
    }
    else if (useAccessorStream ||
        Serialization::ProjectSettingsSerializedBackend::Supports(m_targets) ||
        Serialization::ImporterDraftSerializedBackend::Supports(m_targets) ||
        Serialization::MaterialSerializedBackend::Supports(m_targets))
    {
        // The Inspector presentation is UI-only. Every editable target type is
        // backed by the canonical byte-stream SerializedObject.
        std::shared_ptr<Serialization::ISerializedTargetBackend> backend;
        if (useAccessorStream)
            backend = Serialization::AccessorSerializedBackend::Create();
        else if (Serialization::ProjectSettingsSerializedBackend::Supports(m_targets))
            backend = Serialization::ProjectSettingsSerializedBackend::Create();
        else if (Serialization::ImporterDraftSerializedBackend::Supports(m_targets))
            backend = Serialization::ImporterDraftSerializedBackend::Create();
        else
            backend = Serialization::MaterialSerializedBackend::Create();
        if (!m_serializedObject)
            m_serializedObject = std::make_unique<Serialization::SerializedObject>(m_targets, backend);
        else
            m_serializedObject->Reset(m_targets, backend);
    }
    else
    {
        auto backend = Serialization::NativeReflectionSerializedBackend::Create();
        if (backend->BuildTypeTree(m_targets, Serialization::SerializedInspectorMode::Normal).Succeeded())
        {
            if (!m_serializedObject)
                m_serializedObject = std::make_unique<Serialization::SerializedObject>(m_targets, std::move(backend));
            else
                m_serializedObject->Reset(m_targets, std::move(backend));
        }
        else
        {
            // Unsupported layouts intentionally remain presentation-only and
            // read-only; they must never fall back to callback-backed writes.
            m_serializedObject.reset();
        }
    }

    // Every target kind now gets its body from the same canonical byte-stream
    // cursor sequence. Adapters supplied only the surrounding presentation
    // chrome above; they never provide field rows or write callbacks.
    if (m_serializedObject != nullptr && !m_tree.roots.empty())
    {
        const auto projectedProperties = InspectorProjectionBuilder::BuildPropertyProjection(
            *m_serializedObject,
            m_targets,
            m_searchText);
        if (m_targets.Primary() != nullptr &&
            m_targets.Primary()->kind == InspectorTargetKind::GameObject)
        {
            // The GameObject host consumes active/name/tag/layer itself; retain
            // the projection as auxiliary UI data so those controls resolve
            // through the same SerializedProperty source as regular rows.
            m_tree.auxiliary = projectedProperties;
        }
        else
        {
            for (auto& root : m_tree.roots)
                root.children = projectedProperties;
        }
    }
    m_treeStructureVersion = structureVersion;
    m_registryVersion = registryVersion;
    m_metadataVersion = metadataVersion;
    m_treeDirty = false;
}

void InspectorView::RefreshPrefabOverridesIfNeeded()
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::RefreshPrefabOverrides");
    if (!m_prefabOverridesDirty)
        return;

    if (m_prefabOverrideQuery == nullptr)
        m_prefabOverrideQuery = std::make_unique<PrefabOverrideQuery>();
    RefreshPrefabOverrideStates(m_tree, *m_prefabOverrideQuery);
    m_prefabActions.clear();
    m_prefabActionsResolved = false;
    m_prefabOverridesDirty = false;
}

void InspectorView::DrawToolbar()
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::DrawToolbar");
    const float scale = GetUiScale();
    const float iconWidth = 26.0f * scale;
    const float lockWidth = 32.0f * scale;
    const float menuWidth = 28.0f * scale;
    const auto* target = m_targets.Primary();
    auto* gameObject = GetPrimaryGameObject();
    const auto makeTargetProperty = [target](const char* path)
    {
        PropertyNode property;
        property.path = PropertyPath(path);
        if (target != nullptr)
        {
            property.target = *target;
            SetPropertyNodeTargets(property, {*target});
        }
        return property;
    };

    if (ImGui::BeginTable("##InspectorToolbar", 4, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("##TargetIcon", ImGuiTableColumnFlags_WidthFixed, iconWidth);
        ImGui::TableSetupColumn("##TargetTitle", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##Lock", ImGuiTableColumnFlags_WidthFixed, lockWidth);
        ImGui::TableSetupColumn("##Menu", ImGuiTableColumnFlags_WidthFixed, menuWidth);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        auto activeProperty = makeTargetProperty("GameObject.active");
        activeProperty.fieldName = "active";
        activeProperty.displayName = "Active";
        activeProperty.valueType = NLS_TYPEOF(bool);
        activeProperty.kind = InspectorPropertyKind::Boolean;
        if (m_prefabOverrideQuery != nullptr)
            activeProperty.prefabOverride = m_prefabOverrideQuery->GetState(activeProperty);
        const bool activeOverride = activeProperty.prefabOverride != PrefabOverrideState::None;
        if (activeOverride)
        {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        }
        ImGui::BeginDisabled(gameObject == nullptr);
        if (gameObject != nullptr)
        {
            const auto activeEdit = m_propertyEditor.DrawProperty(activeProperty, MakePropertyEditorContext());
            if (!activeEdit.changedPaths.empty()) CommitSerializedEdit();
        }
        else ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(),ImGui::GetFrameHeight()));
        ImGui::EndDisabled();
        if (activeOverride)
        {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        DrawTooltipIfHovered("Active");
        DrawPrefabOverrideMenu(activeProperty, "##PrefabActiveMenu");

        ImGui::TableSetColumnIndex(1);
        const std::string targetName = gameObject != nullptr
            ? gameObject->GetName()
            : (target != nullptr && !target->resourcePath.empty() ? target->resourcePath : "No selection");
        auto nameProperty = makeTargetProperty("GameObject.name");
        nameProperty.fieldName = "name";
        nameProperty.displayName = "Name";
        nameProperty.valueType = NLS_TYPEOF(std::string);
        nameProperty.kind = InspectorPropertyKind::String;
        if (m_prefabOverrideQuery != nullptr)
            nameProperty.prefabOverride = m_prefabOverrideQuery->GetState(nameProperty);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool nameOverride = nameProperty.prefabOverride != PrefabOverrideState::None;
        if (nameOverride)
        {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        }
        ImGui::BeginDisabled(gameObject == nullptr);
        if (gameObject != nullptr)
            (void)m_propertyEditor.DrawProperty(nameProperty, MakePropertyEditorContext());
        else
            ImGui::TextUnformatted(targetName.c_str());
        ImGui::EndDisabled();
        if (nameOverride)
        {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        DrawPrefabOverrideMenu(nameProperty, "##PrefabNameMenu");

        ImGui::TableSetColumnIndex(2);
        if (ImGui::Button(m_locked ? "L" : "U", ImVec2(lockWidth - 2.0f * scale, 0.0f)))
            SetLocked(!m_locked);
        DrawTooltipIfHovered(m_locked ? "Unlock Inspector and follow the current selection" : "Lock Inspector to this target");

        ImGui::TableSetColumnIndex(3);
        if (ImGui::SmallButton("..."))
            ImGui::OpenPopup("##InspectorOverflow");
        if (ImGui::BeginPopup("##InspectorOverflow"))
        {
            auto* undo = NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>()
                ? &NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>()
                : nullptr;
            const std::string undoLabel = undo != nullptr && !undo->GetUndoLabel().empty()
                ? "Undo " + undo->GetUndoLabel()
                : "Undo";
            const std::string redoLabel = undo != nullptr && !undo->GetRedoLabel().empty()
                ? "Redo " + undo->GetRedoLabel()
                : "Redo";
            const auto shortcutLabel = [](const char* commandId, const char* fallback)
            {
                return NLS::Core::ServiceLocator::Contains<Shortcuts::EditorShortcutService>()
                    ? NLS::Core::ServiceLocator::Get<Shortcuts::EditorShortcutService>()
                        .GetBindingDisplayText(commandId)
                    : std::string(fallback);
            };
            const auto undoShortcut = shortcutLabel("edit.undo", "Ctrl + Z");
            const auto redoShortcut = shortcutLabel("edit.redo", "Ctrl + Y");
            const bool canUndo = undo != nullptr && undo->CanUndo();
            const bool canRedo = undo != nullptr && undo->CanRedo();
            const std::string undoUnavailableReason = undo != nullptr
                ? undo->GetUndoUnavailableReason()
                : "The global Undo service is unavailable.";
            const std::string redoUnavailableReason = undo != nullptr
                ? undo->GetRedoUnavailableReason()
                : "The global Undo service is unavailable.";
            if (ImGui::MenuItem(
                    undoLabel.c_str(),
                    undoShortcut.c_str(),
                    false,
                    canUndo))
            {
                if (undo->RequestUndo().Succeeded())
                {
                    Refresh();
                    StructureChangedEvent.Invoke();
                }
            }
            DrawTooltipIfHovered(canUndo ? std::string {} : undoUnavailableReason);
            if (ImGui::MenuItem(
                    redoLabel.c_str(),
                    redoShortcut.c_str(),
                    false,
                    canRedo))
            {
                if (undo->RequestRedo().Succeeded())
                {
                    Refresh();
                    StructureChangedEvent.Invoke();
                }
            }
            DrawTooltipIfHovered(canRedo ? std::string {} : redoUnavailableReason);
            const bool hasPrefabActions = target != nullptr &&
                m_prefabOverrideQuery != nullptr &&
                m_prefabOverrideQuery->HasInstance(*target);
            if (hasPrefabActions && ImGui::BeginMenu("Overrides"))
            {
                if (!m_prefabActionsResolved)
                {
                    m_prefabActions = GetPrefabInspectorActions(m_targets);
                    m_prefabActionsResolved = true;
                }
                for (const auto& action : m_prefabActions)
                {
                    ImGui::PushID(static_cast<int>(action.kind));
                    ImGui::BeginDisabled(!action.enabled);
                    if (ImGui::MenuItem(action.label.c_str()))
                    {
                        std::string error;
                        if (ExecutePrefabInspectorAction(m_targets, action.kind, error))
                        {
                            m_prefabActionError.clear();
                            Refresh();
                            StructureChangedEvent.Invoke();
                        }
                        else
                        {
                            m_prefabActionError = error.empty()
                                ? "The prefab override operation could not be completed."
                                : std::move(error);
                        }
                    }
                    ImGui::EndDisabled();
                    DrawTooltipIfHovered(action.tooltip);
                    ImGui::PopID();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Refresh"))
                Refresh();
            ImGui::EndPopup();
        }
        DrawTooltipIfHovered("Inspector options");
        ImGui::EndTable();
    }

    if (gameObject != nullptr && ImGui::BeginTable("##InspectorTargetMetadata", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("##InspectorTargetTag", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("##InspectorTargetLayer", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        const auto tagChoices = Settings::TagLayerSettings::BuildTagChoices();
        auto tagProperty = makeTargetProperty("GameObject.tag");
        tagProperty.fieldName = "tag";
        tagProperty.displayName = "Tag";
        tagProperty.valueType = NLS_TYPEOF(std::string);
        tagProperty.kind = InspectorPropertyKind::Choice;
        for (const auto& [index, tag] : tagChoices)
            tagProperty.choices.push_back({tag, tag});
        if (m_prefabOverrideQuery != nullptr)
            tagProperty.prefabOverride = m_prefabOverrideQuery->GetState(tagProperty);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool tagOverride = tagProperty.prefabOverride != PrefabOverrideState::None;
        if (tagOverride)
        {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        }
        (void)m_propertyEditor.DrawProperty(tagProperty, MakePropertyEditorContext());
        if (tagOverride)
        {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        DrawTooltipIfHovered("Tag");
        DrawPrefabOverrideMenu(tagProperty, "##PrefabTagMenu");

        ImGui::TableSetColumnIndex(1);
        const auto layerChoices = Settings::TagLayerSettings::BuildLayerChoices();
        auto layerProperty = makeTargetProperty("GameObject.layer");
        layerProperty.fieldName = "layer";
        layerProperty.displayName = "Layer";
        layerProperty.valueType = NLS_TYPEOF(int32_t);
        layerProperty.kind = InspectorPropertyKind::Choice;
        for (const auto& [index, label] : layerChoices)
        {
            InspectorPropertyChoice choice{std::to_string(index), label};
            choice.typedValue = static_cast<int32_t>(index);
            layerProperty.choices.push_back(std::move(choice));
        }
        if (m_prefabOverrideQuery != nullptr)
            layerProperty.prefabOverride = m_prefabOverrideQuery->GetState(layerProperty);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool layerOverride = layerProperty.prefabOverride != PrefabOverrideState::None;
        if (layerOverride)
        {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        }
        (void)m_propertyEditor.DrawProperty(layerProperty, MakePropertyEditorContext());
        if (layerOverride)
        {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        DrawTooltipIfHovered("Layer");
        DrawPrefabOverrideMenu(layerProperty, "##PrefabLayerMenu");
        ImGui::EndTable();
    }

    std::vector<char> searchBuffer;
    CopyToBuffer(m_searchText, searchBuffer, 160u);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##InspectorSearch", "Search properties", searchBuffer.data(), searchBuffer.size()))
    {
        m_searchText = searchBuffer.data();
        m_treeDirty = true;
    }
    ImGui::Separator();
}

void InspectorView::DrawGameObject(const PropertyNode& gameObjectNode)
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::DrawGameObject");
    size_t componentIndex = 0u;
    for (const auto& component : gameObjectNode.children)
    {
        // GameObject header properties live in the same canonical serialized
        // tree but are rendered by DrawToolbar. Only component nodes belong in
        // the component stack below the header.
        if (!component.isComponent)
            continue;
        DrawComponent(component, componentIndex++);
    }

    DrawAddComponent();
}

void InspectorView::DrawComponentEditorBody(const PropertyNode& componentNode)
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::DrawComponentEditorBody");
    auto* component = dynamic_cast<Engine::Components::Component*>(
        NativeReflectionAdapter::ResolveObject(componentNode.target));
    if (component == nullptr)
    {
        ImGui::TextDisabled("Component target is no longer available");
        return;
    }

    // Every C#/Lua script component shares one native reflection type, so a
    // scripted editor for a Behaviour is resolved by the script's own type name
    // rather than by the component type.
    std::string_view scriptTypeName;
    if (auto* scriptComponent = dynamic_cast<NLS::Scripting::ScriptComponent*>(component))
    {
        if (const auto* descriptor = scriptComponent->GetScriptTypeDescriptor())
            scriptTypeName = descriptor->name;
    }
    const auto registeredEditor = scriptTypeName.empty()
        ? InspectorRegistry::Get().ResolveComponentEditor(component->GetType())
        : InspectorRegistry::Get().ResolveScriptComponentEditor(scriptTypeName);
    const bool multiTarget = GetPropertyNodeTargets(componentNode).size() > 1u || !m_targets.IsSingleTarget();
    const ComponentEditorRegistration defaultEditor {
        component->GetType(),
        "__default_serialized_component_editor",
        true,
        true,
        true,
        [] { return std::make_unique<DefaultComponentEditor>(); }};
    // A custom editor without [CanEditMultipleObjects] falls back to the
    // generic serialized inspector instead of reviving adapter-built rows.
    const auto& registration = registeredEditor.has_value() &&
            (!multiTarget || registeredEditor->supportsMultiObject)
        ? *registeredEditor
        : defaultEditor;

    auto targetBindings = GetPropertyNodeTargets(componentNode);
    if (targetBindings.empty() && componentNode.target.IsValid())
        targetBindings = {componentNode.target};
    const auto key = MakeComponentEditorKey(componentNode, registration, targetBindings);
    m_liveComponentEditorKeys.insert(key);
    auto editorEntry = m_componentEditors.find(key);
    const bool recreate = editorEntry == m_componentEditors.end() ||
        editorEntry->second.registrationIdentifier != registration.identifier ||
        !(editorEntry->second.componentType == component->GetType()) ||
        editorEntry->second.targets != targetBindings;
    if (recreate)
    {
        if (editorEntry != m_componentEditors.end() && editorEntry->second.editor != nullptr)
            editorEntry->second.editor->OnDisable();

        ComponentEditorEntry entry;
        entry.registrationIdentifier = registration.identifier;
        entry.componentType = component->GetType();
        entry.targets = targetBindings;
        entry.editor = registration.create ? registration.create() : nullptr;
        if (entry.editor != nullptr)
        {
            // external editor initializes SerializedObject before invoking OnEnable. Do
            // the same here so custom editors can cache only stable property
            // paths/metadata during setup, never a live component pointer.
            entry.editor->UpdateSerializedObject(componentNode);
            entry.editor->OnEnable();
        }
        editorEntry = m_componentEditors.insert_or_assign(key, std::move(entry)).first;
    }

    if (editorEntry == m_componentEditors.end() || editorEntry->second.editor == nullptr)
    {
        ImGui::TextDisabled("Component editor could not be created");
        return;
    }

    auto& editor = *editorEntry->second.editor;
    const uint64_t previousSerializedGeneration = editor.GetSerializedObject().GetGeneration();
    editor.UpdateSerializedObject(componentNode);
    if (!recreate &&
        previousSerializedGeneration != 0u &&
        previousSerializedGeneration != editor.GetSerializedObject().GetGeneration())
    {
        // Cached SerializedProperty handles are generation-scoped. Re-run the
        // engine-style lifecycle when a script reload or structural rebuild
        // invalidates that generation so custom editors can reacquire their
        // fields from serializedObject in OnEnable().
        editor.OnDisable();
        editor.OnEnable();
    }
    InspectorTargetSet componentTargets;
    componentTargets.targets = targetBindings;
    componentTargets.Normalize();
    const auto componentProperties = InspectorProjectionBuilder::BuildPropertyProjection(
        editor.GetSerializedObject(),
        componentTargets,
        m_searchText);

    // A ComponentEditor is an immediate-mode engine-style editor.  Its
    // PropertyField calls render into this one shared table immediately, so
    // labels, drawers, search and edit sessions stay in the same order as the
    // custom editor code.  Nothing is queued for a second pass.
    struct ComponentEditorLayoutState
    {
        bool compact = false;
        bool tableOpen = false;
        float scale = 1.0f;
    } layoutState;
    layoutState.scale = GetUiScale();
    layoutState.compact = BuildInspectorPropertiesLayout(
        ImGui::GetContentRegionAvail().x,
        layoutState.scale,
        m_labelColumnLogicalWidth).compact;

    const auto ensureTable = [&]()
    {
        if (layoutState.compact || layoutState.tableOpen)
            return true;
        const auto layout = BuildInspectorPropertiesLayout(
            ImGui::GetContentRegionAvail().x,
            layoutState.scale,
            m_labelColumnLogicalWidth);
        if (!ImGui::BeginTable(
                "##ComponentEditorProperties",
                2,
                ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_BordersInnerV))
        {
            return false;
        }
        ImGui::TableSetupColumn(
            "##ComponentEditorLabel",
            ImGuiTableColumnFlags_WidthFixed,
            layout.labelLogicalWidth * layoutState.scale);
        ImGui::TableSetupColumn("##ComponentEditorValue", ImGuiTableColumnFlags_WidthStretch);
        layoutState.tableOpen = true;
        return true;
    };
    const auto closeTable = [&]()
    {
        if (layoutState.tableOpen)
        {
            ImGui::EndTable();
            layoutState.tableOpen = false;
        }
    };

    const auto applyOptions = [this](PropertyNode property, const ComponentEditorPropertyOptions& options)
    {
        if (!options.label.empty())
            property.displayName = options.label;
        if (!options.tooltip.empty())
            property.metadata.tooltip = options.tooltip;
        if (options.kind.has_value())
            property.kind = *options.kind;
        if (!options.choices.empty())
            property.choices = options.choices;
        if (options.forceVisible)
            property.metadata.hideInInspector = false;
        if (!options.includeChildren)
            property.children.clear();
        if (m_prefabOverrideQuery != nullptr)
            property.prefabOverride = m_prefabOverrideQuery->GetState(property);
        return property;
    };

    ComponentEditorContextCallbacks callbacks;
    callbacks.drawProperty = [&](const Serialization::SerializedProperty& serializedProperty, const ComponentEditorPropertyOptions& options)
    {
        const auto* source = FindComponentPropertyNode(
            componentProperties,
            serializedProperty.GetPropertyPath());
        if (source == nullptr)
            return false;
        auto property = applyOptions(*source, options);
        const auto previousOverride = m_serializedDrawOverride;
        m_serializedDrawOverride = SerializedDrawOverride {serializedProperty, &editor.GetSerializedObject(), property.path};
        if (layoutState.compact)
        {
            DrawPropertyCompact(property);
            m_serializedDrawOverride = previousOverride;
            return true;
        }
        if (!ensureTable())
        {
            m_serializedDrawOverride = previousOverride;
            return false;
        }
        DrawPropertyRow(property);
        m_serializedDrawOverride = previousOverride;
        return true;
    };
    callbacks.drawButton = [&](const ComponentEditorAction& action)
    {
        bool clicked = false;
        if (layoutState.compact)
        {
            ImGui::Separator();
            ImGui::BeginDisabled(!action.enabled);
            clicked = ImGui::Button(action.label.c_str(), ImVec2(-FLT_MIN, 0.0f));
            ImGui::EndDisabled();
        }
        else if (ensureTable())
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginDisabled(!action.enabled);
            clicked = ImGui::Button(action.label.c_str(), ImVec2(-FLT_MIN, 0.0f));
            ImGui::EndDisabled();
        }
        DrawTooltipIfHovered(action.tooltip);
        if (clicked && action.execute)
        {
            action.execute();
            Refresh();
            StructureChangedEvent.Invoke();
        }
        return clicked;
    };
    callbacks.drawHelpBox = [&](const ComponentEditorMessage& message)
    {
        const ImVec4 color = message.kind == ComponentEditorMessageKind::Error
            ? ImVec4(0.90f, 0.38f, 0.34f, 1.0f)
            : message.kind == ComponentEditorMessageKind::Warning
                ? ImVec4(0.95f, 0.72f, 0.28f, 1.0f)
                : ImVec4(0.60f, 0.75f, 0.90f, 1.0f);
        if (layoutState.compact)
        {
            ImGui::TextColored(color, "%s", message.text.c_str());
        }
        else if (ensureTable())
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(color, "%s", message.text.c_str());
        }
    };
    callbacks.applyModifiedProperties = [&editor]
    {
        return editor.GetSerializedObject().ApplyModifiedProperties();
    };
    callbacks.discardModifiedProperties = [&editor]
    {
        const bool hadChanges = editor.GetSerializedObject().HasModifiedProperties();
        editor.GetSerializedObject().DiscardModifiedProperties();
        return hadChanges;
    };

    ComponentEditorContext context(
        editor.GetSerializedObject(),
        componentTargets,
        m_searchText,
        std::move(callbacks));
    editor.BeginInspectorGUI(context);
    editor.OnInspectorGUI();
    editor.EndInspectorGUI();
    closeTable();
}

void InspectorView::DrawComponent(const PropertyNode& componentNode, const size_t componentIndex)
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::DrawComponent");
    auto* component = dynamic_cast<Engine::Components::Component*>(NativeReflectionAdapter::ResolveObject(componentNode.target));
    if (component == nullptr)
        return;

    const float scale = GetUiScale();
    const std::string title = GetComponentTitle(componentNode);
    ImGui::PushID(componentNode.path.ToString().c_str());
    if (ImGui::BeginTable("##ComponentHeader", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("##Enabled", ImGuiTableColumnFlags_WidthFixed, 28.0f * scale);
        ImGui::TableSetupColumn("##ComponentName", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##ComponentMenu", ImGuiTableColumnFlags_WidthFixed, 28.0f * scale);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        PropertyNode enabledProperty;
        const auto enabledNode = std::find_if(
            componentNode.children.begin(),
            componentNode.children.end(),
            [](const PropertyNode& candidate) { return candidate.fieldName == "enabled"; });
        if (enabledNode != componentNode.children.end())
        {
            enabledProperty = *enabledNode;
        }
        else
        {
            // A missing bridge field is a schema problem, not permission to
            // write the component directly from the header.
            enabledProperty.path = componentNode.path.Field("enabled");
            enabledProperty.fieldName = "enabled";
            enabledProperty.displayName = "Enabled";
            enabledProperty.kind = InspectorPropertyKind::Boolean;
            enabledProperty.valueType = NLS_TYPEOF(bool);
            enabledProperty.target = componentNode.target;
            enabledProperty.readOnly = true;
            enabledProperty.diagnostic = "Component enabled state is unavailable in the serialized schema.";
        }
        Serialization::SerializedTargetSet componentHeaderTargets;
        componentHeaderTargets.targets = GetPropertyNodeTargets(componentNode);
        if (componentHeaderTargets.targets.empty() && componentNode.target.IsValid())
            componentHeaderTargets.targets = {componentNode.target};
        componentHeaderTargets.Normalize();
        std::unique_ptr<Serialization::SerializedObject> componentHeaderSerialized;
        if (Serialization::AccessorSerializedBackend::SupportsComponentHeader(componentHeaderTargets))
        {
            componentHeaderSerialized = std::make_unique<Serialization::SerializedObject>(
                componentHeaderTargets,
                Serialization::AccessorSerializedBackend::CreateComponentHeader());
        }
        const auto serializedEnabled = componentHeaderSerialized != nullptr
            ? componentHeaderSerialized->FindProperty("enabled")
            : ResolveSerializedProperty(enabledProperty);
        if (serializedEnabled)
            enabledProperty.valueState = serializedEnabled.GetValueState();
        if (m_prefabOverrideQuery != nullptr)
            enabledProperty.prefabOverride = m_prefabOverrideQuery->GetState(enabledProperty);
        enabledProperty.readOnly = componentNode.isTransform;
        const bool enabledOverride = enabledProperty.prefabOverride != PrefabOverrideState::None;
        if (enabledOverride)
        {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        }
        const bool enabledWritable = serializedEnabled && serializedEnabled.IsEditable();
        ImGui::BeginDisabled(enabledProperty.readOnly || !enabledWritable);
        auto enabledContext = MakePropertyEditorContext(componentHeaderSerialized.get());
        enabledContext.resolve = [serializedEnabled](const PropertyNode&)
        { return serializedEnabled; };
        const auto enabledEdit = m_propertyEditor.DrawProperty(enabledProperty, enabledContext);
        if (!enabledEdit.changedPaths.empty())
        {
            if (componentHeaderSerialized)
                (void)componentHeaderSerialized->ApplyModifiedProperties();
            else
                (void)CommitSerializedEdit();
        }
        ImGui::EndDisabled();
        if (enabledOverride)
        {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        DrawTooltipIfHovered(componentNode.isTransform
            ? "Transform is always enabled"
            : (enabledProperty.valueState == PropertyValueState::Mixed
                ? "Selected targets have different enabled states. Clicking applies one state to all of them."
                : "Enable component"));
        DrawPrefabOverrideMenu(enabledProperty, "##PrefabEnabledMenu");

        ImGui::TableSetColumnIndex(1);
        const bool hasStructuralOverride = componentNode.prefabOverride == PrefabOverrideState::StructuralOverride;
        if (hasStructuralOverride)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
        // A table cannot outlive a tree node's ID scope. The component body is
        // drawn after this header table, so keep the foldout state without
        // letting TreeNodeEx push an ID that would survive until EndTable().
        const bool opened = ImGui::TreeNodeEx(
            "##ComponentFoldout",
            ImGuiTreeNodeFlags_DefaultOpen |
                ImGuiTreeNodeFlags_SpanAvailWidth |
                ImGuiTreeNodeFlags_NoTreePushOnOpen,
            "%s",
            title.c_str());
        if (hasStructuralOverride)
            ImGui::PopStyleColor();
        if (hasStructuralOverride)
            DrawTooltipIfHovered("This component has a structural prefab override.");

        ImGui::TableSetColumnIndex(2);
        ImGui::BeginDisabled(!m_targets.IsSingleTarget());
        if (ImGui::SmallButton("..."))
            ImGui::OpenPopup("##ComponentMenu");
        ImGui::EndDisabled();
        if (!m_targets.IsSingleTarget())
            DrawTooltipIfHovered("Component structure editing requires a single selected GameObject");
        DrawComponentMenu(componentNode, componentIndex);
        ImGui::EndTable();

        if (opened)
        {
            ImGui::Indent();
            DrawComponentEditorBody(componentNode);
            ImGui::Unindent();
        }
    }
    ImGui::Spacing();
    ImGui::PopID();
}

void InspectorView::DrawPrefabOverrideMenu(const PropertyNode& property, const char* popupId)
{
    if (property.prefabOverride == PrefabOverrideState::None)
        return;

    ImGui::PushID(property.path.ToString().c_str());
    if (ImGui::BeginPopupContextItem(popupId))
    {
        DrawPrefabOverrideMenuItems(property);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void InspectorView::DrawPrefabOverrideMenuItems(const PropertyNode& property)
{
    if(property.prefabOverride==PrefabOverrideState::None) return;
        for (const auto& action : GetPrefabInspectorActions(property))
        {
            ImGui::BeginDisabled(!action.enabled);
            if (ImGui::MenuItem(action.label.c_str()))
            {
                std::string error;
                if (ExecutePrefabInspectorAction(property, action.kind, error))
                {
                    m_prefabActionError.clear();
                    Refresh();
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_prefabActionError = error.empty()
                        ? "The property prefab override operation could not be completed."
                        : std::move(error);
                }
            }
            ImGui::EndDisabled();
            DrawTooltipIfHovered(action.tooltip);
        }
}

void InspectorView::DrawAddComponent()
{
    const bool singleTarget = m_targets.IsSingleTarget();
    auto* gameObject = singleTarget ? GetPrimaryGameObject() : nullptr;
    m_componentPicker.SetTargetGameObject(gameObject);

    ImGui::BeginDisabled(gameObject == nullptr);
    if (ImGui::Button("Add Component", ImVec2(-FLT_MIN, 0.0f)))
    {
        m_componentPicker.SetAnchorRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        m_componentPicker.OpenForGameObject(gameObject);
    }
    ImGui::EndDisabled();
    if (!singleTarget)
        DrawTooltipIfHovered("Adding components requires a single selected GameObject");
    else if (gameObject == nullptr)
        DrawTooltipIfHovered("Select a GameObject to add a component");

    m_componentPicker.Draw();
}

void InspectorView::DrawComponentMenu(const PropertyNode& componentNode, const size_t componentIndex)
{
    auto* component = dynamic_cast<Engine::Components::Component*>(NativeReflectionAdapter::ResolveObject(componentNode.target));
    auto* owner = component != nullptr ? component->gameobject() : nullptr;
    const auto componentIdentifier = component != nullptr
        ? CaptureComponentObjectIdentifier(*component)
        : std::nullopt;
    if (!ImGui::BeginPopup("##ComponentMenu"))
        return;

    if (!m_targets.IsSingleTarget())
    {
        ImGui::TextDisabled("Component structure editing requires a single selected GameObject");
        ImGui::EndPopup();
        return;
    }

    if (componentNode.prefabOverride == PrefabOverrideState::StructuralOverride)
    {
        for (const auto& action : GetPrefabInspectorActions(componentNode))
        {
            ImGui::BeginDisabled(!action.enabled);
            if (ImGui::MenuItem(action.label.c_str()))
            {
                std::string error;
                if (ExecutePrefabInspectorAction(componentNode, action.kind, error))
                {
                    m_prefabActionError.clear();
                    Refresh();
                    StructureChangedEvent.Invoke();
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_prefabActionError = error.empty()
                        ? "The component prefab override operation could not be completed."
                        : std::move(error);
                }
            }
            ImGui::EndDisabled();
            DrawTooltipIfHovered(action.tooltip);
        }
        ImGui::Separator();
    }

    const bool hasComponentTarget = component != nullptr && owner != nullptr && componentIdentifier.has_value();
    const bool protectedComponent = componentNode.isTransform || !hasComponentTarget;
    if (ImGui::MenuItem("Reset", nullptr, false, hasComponentTarget))
    {
        const auto ownerHandle = InspectorTargetHandle::FromObject(*owner, InspectorTargetKind::GameObject);
        const auto before = CaptureComponentStructure(*owner);
        ComponentStructureEditOperation operation;
        operation.kind = ComponentStructureEditKind::Reset;
        operation.componentIdentifier = *componentIdentifier;
        if (ExecuteComponentStructureEdit(
                "Reset " + GetComponentTitle(componentNode),
                ownerHandle,
                before,
                std::move(operation)))
        {
            Refresh();
            StructureChangedEvent.Invoke();
        }
    }
    if (ImGui::MenuItem("Copy", nullptr, false, component != nullptr))
    {
        m_componentClipboard = CaptureComponentSnapshot(*component, componentIndex);
        ImGui::CloseCurrentPopup();
    }
    const bool canPaste = hasComponentTarget && m_componentClipboard.has_value() &&
        component->GetType().IsValid() &&
        component->GetType().GetName() == m_componentClipboard->record.typeName;
    ImGui::BeginDisabled(!canPaste);
    if (ImGui::MenuItem("Paste Values"))
    {
        const auto ownerHandle = InspectorTargetHandle::FromObject(*owner, InspectorTargetKind::GameObject);
        const auto before = CaptureComponentStructure(*owner);
        ComponentStructureEditOperation operation;
        operation.kind = ComponentStructureEditKind::PasteValues;
        operation.componentIdentifier = *componentIdentifier;
        operation.pastedValues = *m_componentClipboard;
        if (ExecuteComponentStructureEdit(
                "Paste " + GetComponentTitle(componentNode) + " Values",
                ownerHandle,
                before,
                std::move(operation)))
        {
            Refresh();
            StructureChangedEvent.Invoke();
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::BeginDisabled(protectedComponent || componentIndex <= 1u);
    if (ImGui::MenuItem("Move Up"))
    {
        const auto ownerHandle = InspectorTargetHandle::FromObject(*owner, InspectorTargetKind::GameObject);
        const auto before = CaptureComponentStructure(*owner);
        ComponentStructureEditOperation operation;
        operation.kind = ComponentStructureEditKind::Move;
        operation.componentIdentifier = *componentIdentifier;
        operation.destinationIndex = componentIndex - 1u;
        if (ExecuteComponentStructureEdit(
                "Move " + GetComponentTitle(componentNode) + " Up",
                ownerHandle,
                before,
                std::move(operation)))
        {
            Refresh();
            StructureChangedEvent.Invoke();
        }
    }
    ImGui::EndDisabled();
    const size_t componentCount = owner != nullptr ? owner->GetComponents().size() : 0u;
    ImGui::BeginDisabled(protectedComponent || componentIndex + 1u >= componentCount);
    if (ImGui::MenuItem("Move Down"))
    {
        const auto ownerHandle = InspectorTargetHandle::FromObject(*owner, InspectorTargetKind::GameObject);
        const auto before = CaptureComponentStructure(*owner);
        ComponentStructureEditOperation operation;
        operation.kind = ComponentStructureEditKind::Move;
        operation.componentIdentifier = *componentIdentifier;
        operation.destinationIndex = componentIndex + 1u;
        if (ExecuteComponentStructureEdit(
                "Move " + GetComponentTitle(componentNode) + " Down",
                ownerHandle,
                before,
                std::move(operation)))
        {
            Refresh();
            StructureChangedEvent.Invoke();
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    const std::string removeBlockReason = protectedComponent
        ? (componentNode.isTransform ? "Transform cannot be removed" : "The component target is no longer available")
        : Panels::ComponentSearchPanel::GetRemovalBlockReason(component);
    ImGui::BeginDisabled(!removeBlockReason.empty());
    if (ImGui::MenuItem("Remove"))
    {
        const auto ownerHandle = InspectorTargetHandle::FromObject(*owner, InspectorTargetKind::GameObject);
        const auto before = CaptureComponentStructure(*owner);
        ComponentStructureEditOperation operation;
        operation.kind = ComponentStructureEditKind::Remove;
        operation.componentIdentifier = *componentIdentifier;
        if (ExecuteComponentStructureEdit(
                "Remove " + GetComponentTitle(componentNode),
                ownerHandle,
                before,
                std::move(operation)))
        {
            Refresh();
            StructureChangedEvent.Invoke();
        }
    }
    ImGui::EndDisabled();
    if (!removeBlockReason.empty())
        DrawTooltipIfHovered(removeBlockReason);
    ImGui::EndPopup();
}

void InspectorView::DrawTargetActions()
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::DrawTargetActions");
    const auto* adapter = InspectorRegistry::Get().ResolveTargetAdapter(m_targets);
    if (adapter == nullptr)
        return;

    const auto actions = adapter->GetActions(m_targets);
    if (actions.empty())
        return;

    ImGui::Separator();
    const float scale = GetUiScale();
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    std::vector<float> actionLabelWidths;
    actionLabelWidths.reserve(actions.size());
    for (const auto& action : actions)
        actionLabelWidths.push_back(ImGui::CalcTextSize(action.label.c_str()).x);
    const auto actionLayout = BuildInspectorActionLayout(
        availableWidth,
        scale,
        actionLabelWidths,
        ImGui::GetStyle().ItemSpacing.x,
        ImGui::GetStyle().FramePadding.x * 2.0f);
    const bool stackActions = actionLayout.stacked;
    for (size_t index = 0u; index < actions.size(); ++index)
    {
        const auto& action = actions[index];
        ImGui::PushID(static_cast<int>(action.kind));
        ImGui::BeginDisabled(!action.enabled);
        const float buttonWidth = (std::max)(1.0f, actionLayout.buttons[index].width);
        if (ImGui::Button(action.label.c_str(), ImVec2(buttonWidth, 0.0f)))
        {
            std::string error;
            if (adapter->ExecuteAction(m_targets, action.kind, error))
            {
                m_targetActionError.clear();
                Refresh();
            }
            else
            {
                m_targetActionError = error.empty() ? "The requested action could not be completed." : std::move(error);
            }
        }
        ImGui::EndDisabled();
        DrawTooltipIfHovered(action.tooltip);
        ImGui::PopID();
        if (!stackActions && index + 1u < actions.size())
            ImGui::SameLine();
    }

    if (!m_targetActionError.empty())
        ImGui::TextColored(ImVec4(0.90f, 0.38f, 0.34f, 1.0f), "%s", m_targetActionError.c_str());
}

void InspectorView::DrawMissingTarget()
{
    if (m_targets.Empty())
        ImGui::TextDisabled("No object or asset selected");
    else
        ImGui::TextDisabled("The selected target is no longer available");
}

Serialization::SerializedProperty InspectorView::ResolveSerializedProperty(
    const PropertyNode& property) const
{
    if (m_serializedDrawOverride.has_value() && m_serializedDrawOverride->property)
    {
        // The supplied cursor binds the field passed to PropertyField. Nested
        // drawers must resolve their own cursor rather than reuse that parent.
        const auto& binding=*m_serializedDrawOverride;
        if(property.path==binding.projectionPath) return binding.property;
        if(binding.owner)
            if(auto exact=binding.owner->FindProperty(property.path.ToString())) return exact;
        const auto& prefix=binding.projectionPath.ToString();
        const auto& path=property.path.ToString();
        if(path.size()>prefix.size() && path.starts_with(prefix) && path[prefix.size()]=='.')
            return binding.property.FindPropertyRelative(std::string_view(path).substr(prefix.size()+1));
        return {};
    }
    auto* owner = m_serializedObject.get();
    if (!owner || property.path.Empty())
        return {};
    const auto path = property.path.ToString();
    if (auto exact = owner->FindProperty(path); exact)
        return exact;

    // Projection paths can be GameObject/component-qualified while the
    // canonical TypeTree keeps component-local external editor property paths.  Resolve
    // a suffix from the stream instead of reviving a callback binding.
    for (size_t separator = path.find('.'); separator != std::string::npos;
         separator = path.find('.', separator + 1u))
    {
        if (auto relative = owner->FindProperty(path.substr(separator + 1u)); relative)
            return relative;
    }
    return {};
}

bool InspectorView::PreviewSerializedProperty(
    const PropertyNode& property,
    meta::Variant value,
    std::string actionLabel)
{
    if (auto serializedProperty = ResolveSerializedProperty(property);
        serializedProperty)
    {
        const bool changed = serializedProperty.SetValue(value);
        if (changed)
            NotifyPropertyChanged(property);
        return changed;
    }

    // Every production Inspector control is backed by a real stream property.
    // A missing cursor is a stale/unsupported schema, never permission to
    // resurrect the retired UI callback path.
    return false;
}

bool InspectorView::CommitSerializedEdit()
{
    auto* serializedOwner = m_serializedDrawOverride.has_value() ? m_serializedDrawOverride->owner : m_serializedObject.get();
    return serializedOwner != nullptr && serializedOwner->HasModifiedProperties() &&
        serializedOwner->ApplyModifiedProperties();
}

void InspectorView::CommitOrCancelAfterItem(const PropertyNode& property)
{
    (void)m_propertyEditor.FinishItem(property, MakePropertyEditorContext());
}

void InspectorView::NotifyPropertyChanged(const PropertyNode& property)
{
    m_prefabOverridesDirty = true;
    if (m_prefabOverrideQuery != nullptr)
        m_prefabOverrideQuery->InvalidateAll();
    PropertyChangedEvent.Invoke(property);
}

float InspectorView::GetUiScale() const
{
    if (NLS::Core::ServiceLocator::Contains<Core::Context>())
    {
        const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
        if (context.uiManager)
            return (std::max)(0.75f, context.uiManager->GetScale());
    }
    return 1.0f;
}

bool InspectorView::UseCompactLayout() const
{
    return BuildInspectorPropertiesLayout(ImGui::GetContentRegionAvail().x, GetUiScale()).compact;
}

void InspectorView::ClearComponentEditors()
{
    for (auto& [key, entry] : m_componentEditors)
    {
        if (entry.editor != nullptr)
            entry.editor->OnDisable();
    }
    m_componentEditors.clear();
    m_liveComponentEditorKeys.clear();
}

void InspectorView::PruneComponentEditors()
{
    for (auto iterator = m_componentEditors.begin(); iterator != m_componentEditors.end();)
    {
        if (m_liveComponentEditorKeys.find(iterator->first) != m_liveComponentEditorKeys.end())
        {
            ++iterator;
            continue;
        }

        if (iterator->second.editor != nullptr)
            iterator->second.editor->OnDisable();
        iterator = m_componentEditors.erase(iterator);
    }
}

std::string InspectorView::GetComponentTitle(const PropertyNode& componentNode) const
{
    if (auto* component = dynamic_cast<Engine::Components::Component*>(
            NativeReflectionAdapter::ResolveObject(componentNode.target)))
    {
        if (auto* script = dynamic_cast<NLS::Scripting::ScriptComponent*>(component))
        {
            const auto scriptTitle = ScriptSerializedAdapter::GetComponentDisplayName(*script);
            if (!scriptTitle.empty())
                return scriptTitle;
        }
    }
    return componentNode.displayName.empty() ? std::string("Component") : componentNode.displayName;
}

} // namespace NLS::Editor::Inspector
