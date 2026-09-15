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
#include "Settings/TagLayerSettings.h"
#include "Shortcuts/EditorShortcutService.h"
#include "EditorProject_MetaGenerated.h"

namespace NLS::Editor::Inspector
{
namespace
{
constexpr int kMaximumArraySize = 1024;
constexpr int kMaximumNestedDepth = 8;

void EnsureDefaultInspectorAdapters()
{
    static std::once_flag once;
    std::call_once(once, []
    {
        InspectorRegistry::Get().RegisterTargetAdapter(std::make_shared<AssetTargetAdapter>());
        InspectorRegistry::Get().RegisterTargetAdapter(std::make_shared<ProjectSettingsTargetAdapter>());
        InspectorRegistry::Get().RegisterTargetAdapter(std::make_shared<NativeReflectionAdapter>());
    });
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

struct InspectorFloatComponentLabel
{
    const char* name = "";
    const char* tooltip = "";
    ImVec4 color {};
};

const std::array<InspectorFloatComponentLabel, 4u> kInspectorVectorComponentLabels {{
    {"X", "X axis", ImVec4(0.91f, 0.31f, 0.31f, 1.0f)},
    {"Y", "Y axis", ImVec4(0.35f, 0.78f, 0.38f, 1.0f)},
    {"Z", "Z axis", ImVec4(0.37f, 0.61f, 0.95f, 1.0f)},
    {"W", "W component", ImVec4(0.74f, 0.67f, 0.94f, 1.0f)},
}};

const std::array<InspectorFloatComponentLabel, 4u> kInspectorColorComponentLabels {{
    {"R", "Red channel", ImVec4(0.91f, 0.31f, 0.31f, 1.0f)},
    {"G", "Green channel", ImVec4(0.35f, 0.78f, 0.38f, 1.0f)},
    {"B", "Blue channel", ImVec4(0.37f, 0.61f, 0.95f, 1.0f)},
    {"A", "Alpha channel", ImVec4(0.82f, 0.84f, 0.88f, 1.0f)},
}};

const std::array<InspectorFloatComponentLabel, 4u> kInspectorRectComponentLabels {{
    {"X", "Horizontal position", ImVec4(0.91f, 0.31f, 0.31f, 1.0f)},
    {"Y", "Vertical position", ImVec4(0.35f, 0.78f, 0.38f, 1.0f)},
    {"W", "Width", ImVec4(0.37f, 0.61f, 0.95f, 1.0f)},
    {"H", "Height", ImVec4(0.74f, 0.67f, 0.94f, 1.0f)},
}};

void DrawInspectorComponentLabel(
    const InspectorFloatComponentLabel& label,
    const float width)
{
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, ImGui::GetFrameHeight());
    ImGui::InvisibleButton("##ComponentLabel", size);

    const ImVec2 maximum = ImGui::GetItemRectMax();
    const ImVec2 textSize = ImGui::CalcTextSize(label.name);
    const ImVec2 textPosition(
        minimum.x + (size.x - textSize.x) * 0.5f,
        minimum.y + (size.y - textSize.y) * 0.5f);
    const ImVec4 fill(label.color.x, label.color.y, label.color.z, 0.20f);
    const ImVec4 border(label.color.x, label.color.y, label.color.z, 0.52f);
    const auto* style = &ImGui::GetStyle();
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(minimum, maximum, ImGui::GetColorU32(fill), style->FrameRounding);
    drawList->AddRect(minimum, maximum, ImGui::GetColorU32(border), style->FrameRounding);
    drawList->AddText(textPosition, ImGui::GetColorU32(label.color), label.name);
    DrawTooltipIfHovered(label.tooltip);
}

bool DrawLabeledFloatComponents(
    const std::string& id,
    float* values,
    const size_t componentCount,
    const InspectorFloatComponentLabel* labels,
    const float speed,
    const float uiScale,
    bool* editCompleted = nullptr)
{
    if (values == nullptr || labels == nullptr || componentCount == 0u)
        return false;

    const float scale = (std::max)(uiScale, 0.01f);
    const float labelWidth = (std::max)(
        20.0f * scale,
        ImGui::CalcTextSize("W").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const auto layout = BuildInspectorMultiFieldLayout(
        ImGui::GetContentRegionAvail().x,
        scale,
        componentCount,
        spacing,
        labelWidth);

    bool changed = false;
    ImGui::PushID(id.c_str());
    for (size_t index = 0u; index < componentCount; ++index)
    {
        if (!layout.stacked && index > 0u)
            ImGui::SameLine(0.0f, spacing);

        ImGui::BeginGroup();
        ImGui::PushID(static_cast<int>(index));
        DrawInspectorComponentLabel(labels[index], labelWidth);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetNextItemWidth(layout.inputWidth);
        changed = ImGui::DragFloat("##Value", &values[index], speed, 0.0f, 0.0f, "%.3f") || changed;
        if (editCompleted != nullptr && ImGui::IsItemDeactivatedAfterEdit())
            *editCompleted = true;
        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::PopID();
    return changed;
}

bool DrawLabeledColorComponents(
    const std::string& id,
    float* values,
    const float uiScale,
    bool* editCompleted = nullptr)
{
    bool changed = DrawLabeledFloatComponents(
        id,
        values,
        kInspectorColorComponentLabels.size(),
        kInspectorColorComponentLabels.data(),
        0.01f,
        uiScale,
        editCompleted);

    const float pickerWidth = ImGui::GetFrameHeight();
    if (ImGui::GetContentRegionAvail().x >= pickerWidth + ImGui::GetStyle().ItemSpacing.x)
        ImGui::SameLine();
    ImGui::PushID(id.c_str());
    changed = ImGui::ColorEdit4(
        "##Picker",
        values,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel) || changed;
    if (editCompleted != nullptr && ImGui::IsItemDeactivatedAfterEdit())
        *editCompleted = true;
    ImGui::PopID();
    return changed;
}

bool ResizeArray(meta::ArrayWrapper& array, const size_t requestedSize)
{
    const size_t size = (std::min)(requestedSize, static_cast<size_t>(kMaximumArraySize));
    bool changed = false;
    while (array.Size() > size && array.CanRemove())
    {
        array.Remove(array.Size() - 1u);
        changed = true;
    }
    if (array.Size() < size && array.CanResize())
    {
        array.Resize(size);
        return true;
    }
    while (array.Size() < size && array.CanInsertDefault())
    {
        array.InsertDefault(array.Size());
        changed = true;
    }
    return changed;
}

bool IsSimpleArraySortType(const InspectorPropertyKind kind)
{
    return IsNumericPropertyKind(kind) || kind == InspectorPropertyKind::String || kind == InspectorPropertyKind::Boolean;
}

bool CompareArrayValues(const meta::Variant& left, const meta::Variant& right, const InspectorPropertyKind kind)
{
    switch (kind)
    {
    case InspectorPropertyKind::SignedInteger8: return left.GetValue<int8_t>() < right.GetValue<int8_t>();
    case InspectorPropertyKind::UnsignedInteger8: return left.GetValue<uint8_t>() < right.GetValue<uint8_t>();
    case InspectorPropertyKind::SignedInteger16: return left.GetValue<int16_t>() < right.GetValue<int16_t>();
    case InspectorPropertyKind::UnsignedInteger16: return left.GetValue<uint16_t>() < right.GetValue<uint16_t>();
    case InspectorPropertyKind::SignedInteger32: return left.GetValue<int32_t>() < right.GetValue<int32_t>();
    case InspectorPropertyKind::UnsignedInteger32: return left.GetValue<uint32_t>() < right.GetValue<uint32_t>();
    case InspectorPropertyKind::SignedInteger64: return left.GetValue<int64_t>() < right.GetValue<int64_t>();
    case InspectorPropertyKind::UnsignedInteger64: return left.GetValue<uint64_t>() < right.GetValue<uint64_t>();
    case InspectorPropertyKind::Float: return left.GetValue<float>() < right.GetValue<float>();
    case InspectorPropertyKind::Double: return left.GetValue<double>() < right.GetValue<double>();
    case InspectorPropertyKind::Boolean: return left.GetValue<bool>() < right.GetValue<bool>();
    case InspectorPropertyKind::String: return left.GetValue<std::string>() < right.GetValue<std::string>();
    default: return false;
    }
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

InspectorView::InspectorView()
{
    EnsureDefaultInspectorAdapters();
    lineBreak = true;
    m_componentAddedListener = m_componentPicker.ComponentAddedEvent += [this]
    {
        Refresh();
        StructureChangedEvent.Invoke();
    };
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
    EditorSelectionService::Get().SelectionChangedEvent -= m_selectionListener;
}

void InspectorView::SetTargets(InspectorTargetSet targets)
{
    targets.Normalize();
    if (targets == m_targets)
        return;

    if (m_serializedObject != nullptr && m_serializedObject->HasModifiedProperties())
        m_serializedObject->DiscardModifiedProperties();
    ClearComponentEditors();
    m_targets = std::move(targets);
    m_treeDirty = true;
    m_prefabOverridesDirty = true;
    m_prefabActions.clear();
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
    if (!m_prefabOverridesDirty)
        return;

    RefreshPrefabOverrideStates(m_tree);
    m_prefabActions = GetPrefabInspectorActions(m_targets);
    m_prefabOverridesDirty = false;
}

void InspectorView::DrawToolbar()
{
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
        activeProperty.prefabOverride = GetPrefabOverrideState(activeProperty);
        const auto activeSerialized = ResolveSerializedProperty(activeProperty);
        const auto activeValue = activeSerialized
            ? activeSerialized.GetValue()
            : meta::Variant(gameObject != nullptr && gameObject->IsSelfActive());
        bool active = activeValue.IsValid() ? activeValue.GetValue<bool>() :
            (gameObject != nullptr && gameObject->IsSelfActive());
        const bool activeOverride = activeProperty.prefabOverride != PrefabOverrideState::None;
        if (activeOverride)
        {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        }
        ImGui::BeginDisabled(gameObject == nullptr);
        if (ImGui::Checkbox("##InspectorActive", &active) && gameObject != nullptr)
        {
            if (PreviewSerializedProperty(activeProperty, meta::Variant(active), "Set Active"))
                CommitSerializedEdit();
        }
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
        nameProperty.prefabOverride = GetPrefabOverrideState(nameProperty);
        const auto nameSerialized = ResolveSerializedProperty(nameProperty);
        const auto nameValue = nameSerialized
            ? nameSerialized.GetValue()
            : meta::Variant(targetName);
        const std::string editableTargetName = nameValue.IsValid() &&
            nameValue.GetType() == NLS_TYPEOF(std::string)
            ? nameValue.GetValue<std::string>()
            : targetName;
        std::vector<char> nameBuffer;
        CopyToBuffer(editableTargetName, nameBuffer, 160u);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool nameOverride = nameProperty.prefabOverride != PrefabOverrideState::None;
        if (nameOverride)
        {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        }
        ImGui::BeginDisabled(gameObject == nullptr);
        if (ImGui::InputTextWithHint("##InspectorTargetName", "Name", nameBuffer.data(), nameBuffer.size()) && gameObject != nullptr)
        {
            PreviewSerializedProperty(nameProperty, meta::Variant(std::string(nameBuffer.data())), "Rename GameObject");
            CommitOrCancelAfterItem(nameProperty);
        }
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
            if (!m_prefabActions.empty() && ImGui::BeginMenu("Overrides"))
            {
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
        const auto serializedTag = ResolveSerializedProperty(tagProperty);
        const auto tagValue = serializedTag
            ? serializedTag.GetValue()
            : meta::Variant(gameObject->GetTag());
        const std::string currentTagValue = tagValue.IsValid() &&
            tagValue.GetType() == NLS_TYPEOF(std::string)
            ? tagValue.GetValue<std::string>()
            : gameObject->GetTag();
        const int currentTag = Settings::TagLayerSettings::FindTagIndex(currentTagValue);
        const auto tagText = Settings::TagLayerSettings::GetTagAt(currentTag);
        tagProperty.prefabOverride = GetPrefabOverrideState(tagProperty);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool tagOverride = tagProperty.prefabOverride != PrefabOverrideState::None;
        if (tagOverride)
        {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        }
        if (ImGui::BeginCombo("##InspectorTag", tagText.c_str()))
        {
            for (const auto& [index, tag] : tagChoices)
            {
                const bool selected = index == currentTag;
                if (ImGui::Selectable(tag.c_str(), selected))
                {
                    if (PreviewSerializedProperty(tagProperty, meta::Variant(tag), "Set Tag"))
                        CommitSerializedEdit();
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
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
        const auto serializedLayer = ResolveSerializedProperty(layerProperty);
        const auto layerValue = serializedLayer
            ? serializedLayer.GetValue()
            : meta::Variant(static_cast<int32_t>(gameObject->GetLayer()));
        const int currentLayer = layerValue.IsValid() ? layerValue.ToInt() : gameObject->GetLayer();
        const auto layerFound = layerChoices.find(currentLayer);
        const auto layerText = layerFound != layerChoices.end() ? layerFound->second : std::string("Layer 0");
        layerProperty.prefabOverride = GetPrefabOverrideState(layerProperty);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool layerOverride = layerProperty.prefabOverride != PrefabOverrideState::None;
        if (layerOverride)
        {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        }
        if (ImGui::BeginCombo("##InspectorLayer", layerText.c_str()))
        {
            for (const auto& [index, label] : layerChoices)
            {
                const bool selected = index == currentLayer;
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    if (PreviewSerializedProperty(layerProperty, meta::Variant(static_cast<int32_t>(index)), "Set Layer"))
                        CommitSerializedEdit();
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
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
        if (property.prefabOverride == PrefabOverrideState::None)
            property.prefabOverride = GetPrefabOverrideState(property);
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
        m_serializedDrawOverride = SerializedDrawOverride {serializedProperty, &editor.GetSerializedObject()};
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
        const auto enabledValue = serializedEnabled
            ? serializedEnabled.GetValue()
            : meta::Variant(component->IsSelfEnabled());
        bool enabled = enabledValue.IsValid() ? enabledValue.GetValue<bool>() : component->IsSelfEnabled();
        if (serializedEnabled)
            enabledProperty.valueState = serializedEnabled.GetValueState();
        enabledProperty.prefabOverride = GetPrefabOverrideState(enabledProperty);
        enabledProperty.readOnly = componentNode.isTransform;
        const bool enabledOverride = enabledProperty.prefabOverride != PrefabOverrideState::None;
        if (enabledOverride)
        {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.70f, 1.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
        }
        const bool enabledWritable = serializedEnabled && serializedEnabled.IsEditable();
        ImGui::BeginDisabled(enabledProperty.readOnly || !enabledWritable);
        if (enabledProperty.valueState == PropertyValueState::Mixed)
        {
            ImGui::TextDisabled("-");
            ImGui::SameLine();
        }
        if (ImGui::Checkbox("##Enabled", &enabled) && !enabledProperty.readOnly)
        {
            if (componentHeaderSerialized != nullptr && serializedEnabled)
            {
                if (serializedEnabled.SetBoolValue(enabled) &&
                    componentHeaderSerialized->ApplyModifiedProperties())
                {
                    NotifyPropertyChanged(enabledProperty);
                }
            }
            else if (PreviewSerializedProperty(enabledProperty, meta::Variant(enabled), "Set " + title + " Enabled"))
            {
                CommitSerializedEdit();
            }
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

void InspectorView::DrawProperties(const std::vector<PropertyNode>& properties)
{
    if (properties.empty())
    {
        ImGui::TextDisabled("No editable properties");
        return;
    }

    const auto layout = BuildInspectorPropertiesLayout(
        ImGui::GetContentRegionAvail().x,
        GetUiScale(),
        m_labelColumnLogicalWidth);
    if (layout.compact)
    {
        const auto drawCompactNodes = [this](auto&& self, const std::vector<PropertyNode>& nodes) -> void
        {
            for (const auto& property : nodes)
            {
                if (property.metadata.hideInInspector)
                    continue;

                const bool isGroup = property.kind == InspectorPropertyKind::Struct &&
                    !property.children.empty();
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

    if (!ImGui::BeginTable("##InspectorProperties", 2,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
    {
        return;
    }

    ImGui::TableSetupColumn("##InspectorLabel", ImGuiTableColumnFlags_WidthFixed, layout.labelLogicalWidth * scale);
    ImGui::TableSetupColumn("##InspectorValue", ImGuiTableColumnFlags_WidthStretch);
    const auto drawRows = [this](auto&& self, const std::vector<PropertyNode>& nodes) -> void
    {
        for (const auto& property : nodes)
        {
            if (property.metadata.hideInInspector)
                continue;

            const bool isGroup = property.kind == InspectorPropertyKind::Struct &&
                !property.children.empty();
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

void InspectorView::DrawPropertyCompact(const PropertyNode& property)
{
    DrawPropertyDecorators(property);
    if (!property.metadata.header.empty())
    {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", property.metadata.header.c_str());
    }
    if (property.metadata.spaceBefore > 0.0f)
        ImGui::Dummy(ImVec2(0.0f, property.metadata.spaceBefore * GetUiScale()));

    DrawPropertyLabel(property, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(-FLT_MIN);
    DrawPropertyValue(property);
    ImGui::Spacing();
}

void InspectorView::DrawPropertyRow(const PropertyNode& property)
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
    DrawPropertyLabel(property, ImGui::GetContentRegionAvail().x);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    DrawPropertyValue(property);
}

void InspectorView::DrawPropertyDecorators(const PropertyNode& property)
{
    for (const auto& registration : InspectorRegistry::Get().ResolveDecoratorDrawers(property))
    {
        if (registration.draw)
            registration.draw({property, GetUiScale()});
    }
}

void InspectorView::DrawPropertyLabel(const PropertyNode& property, const float availableWidth)
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
}

void InspectorView::DrawPrefabOverrideMenu(const PropertyNode& property, const char* popupId)
{
    if (property.prefabOverride == PrefabOverrideState::None)
        return;

    ImGui::PushID(property.path.ToString().c_str());
    if (ImGui::BeginPopupContextItem(popupId))
    {
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
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

bool InspectorView::DrawPropertyValue(const PropertyNode& property)
{
    const auto serializedProperty = ResolveSerializedProperty(property);
    const bool hasSerializedProperty = static_cast<bool>(serializedProperty);
    auto value = hasSerializedProperty ? serializedProperty.GetValue() : meta::Variant {};
    if (!value.IsValid())
    {
        // Status/diagnostic rows intentionally have no stream property and no
        // callback binding. Keep their current state visible rather than
        // rendering a misleading generic "Unavailable" value.
        if (!hasSerializedProperty && property.readOnly && !property.diagnostic.empty())
        {
            ImGui::TextDisabled("%s", property.diagnostic.c_str());
            DrawTooltipIfHovered(property.diagnostic);
            return false;
        }
        ImGui::TextDisabled("Unavailable");
        return false;
    }

    const auto valueState = hasSerializedProperty
        ? serializedProperty.GetValueState()
        : property.valueState;
    if (valueState == PropertyValueState::Invalid)
    {
        ImGui::TextDisabled("Incompatible selection");
        return false;
    }

    ImGui::PushID(property.path.ToString().c_str());
    const bool serializedEditable = hasSerializedProperty && serializedProperty.IsEditable();
    const bool propertyEditable = hasSerializedProperty && serializedEditable;
    ImGui::BeginDisabled(property.readOnly || !propertyEditable);
    bool changed = false;
    bool componentEditCompleted = false;
    const auto id = MakeWidgetId(property.path);
    if (valueState == PropertyValueState::Mixed)
    {
        ImGui::TextDisabled("Mixed");
        DrawTooltipIfHovered("Selected targets have different values. Editing applies the new value to all of them.");
        ImGui::SameLine();
    }

    if (const auto registration = InspectorRegistry::Get().ResolvePropertyDrawer(property);
        registration.has_value() && registration->draw)
    {
        InspectorPropertyDrawerContext context {property, value, id, GetUiScale()};
        auto drawerProperty = serializedProperty;
        context.serializedProperty = hasSerializedProperty ? &drawerProperty : nullptr;
        context.objectReferencePickerState = &m_objectReferencePickerState;
        context.preview = [this, &property, &context](meta::Variant updated, std::string actionLabel)
        {
            if (!PreviewSerializedProperty(property, std::move(updated), std::move(actionLabel)))
                return false;
            context.changed = true;
            return true;
        };
        context.commit = [this] { return CommitSerializedEdit(); };
        if (registration->draw(context))
        {
            changed = context.changed;
            ImGui::EndDisabled();
            CommitOrCancelAfterItem(property);
            ImGui::PopID();
            return changed;
        }
    }

    switch (property.kind)
    {
    case InspectorPropertyKind::Boolean:
    {
        bool edited = value.GetValue<bool>();
        changed = ImGui::Checkbox(id.c_str(), &edited);
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::SignedInteger8:
    {
        int8_t edited = value.GetValue<int8_t>();
        changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_S8, &edited);
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::UnsignedInteger8:
    {
        uint8_t edited = value.GetValue<uint8_t>();
        changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_U8, &edited);
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::SignedInteger16:
    {
        int16_t edited = value.GetValue<int16_t>();
        changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_S16, &edited);
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::UnsignedInteger16:
    {
        uint16_t edited = value.GetValue<uint16_t>();
        changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_U16, &edited);
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::SignedInteger32:
    {
        int32_t edited = value.GetValue<int32_t>();
        changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_S32, &edited);
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::UnsignedInteger32:
    {
        uint32_t edited = value.GetValue<uint32_t>();
        changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_U32, &edited);
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::SignedInteger64:
    {
        int64_t edited = value.GetValue<int64_t>();
        changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_S64, &edited);
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::UnsignedInteger64:
    {
        uint64_t edited = value.GetValue<uint64_t>();
        changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_U64, &edited);
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::Float:
    {
        float edited = value.GetValue<float>();
        if (property.metadata.rangeMin && property.metadata.rangeMax)
            changed = ImGui::SliderFloat(id.c_str(), &edited, *property.metadata.rangeMin, *property.metadata.rangeMax);
        else
            changed = ImGui::DragFloat(id.c_str(), &edited, 0.01f);
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::Double:
    {
        double edited = value.GetValue<double>();
        changed = ImGui::InputDouble(id.c_str(), &edited, 0.01, 0.1, "%.6f");
        if (changed) PreviewValue(property, edited, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::String:
    {
        std::vector<char> text;
        CopyToBuffer(value.GetValue<std::string>(), text, property.metadata.multiline ? 1024u : 256u);
        changed = property.metadata.multiline
            ? ImGui::InputTextMultiline(id.c_str(), text.data(), text.size(), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f))
            : ImGui::InputText(id.c_str(), text.data(), text.size());
        if (changed) PreviewValue(property, std::string(text.data()), "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::Choice:
    {
        const auto currentValue = value.ToString();
        std::string currentLabel = currentValue;
        for (const auto& choice : property.choices)
        {
            const bool selected = choice.typedValue.IsValid()
                ? choice.typedValue.GetType().GetName() == value.GetType().GetName() &&
                    choice.typedValue.ToString() == currentValue
                : choice.value == currentValue;
            if (selected)
            {
                currentLabel = choice.label;
                break;
            }
        }

        if (ImGui::BeginCombo(id.c_str(), currentLabel.c_str()))
        {
            for (const auto& choice : property.choices)
            {
                const bool selected = choice.typedValue.IsValid()
                    ? choice.typedValue.GetType().GetName() == value.GetType().GetName() &&
                        choice.typedValue.ToString() == currentValue
                    : choice.value == currentValue;
                ImGui::BeginDisabled(!choice.enabled);
                const meta::Variant selectedValue = choice.typedValue.IsValid()
                    ? choice.typedValue
                    : meta::Variant(choice.value);
                if (ImGui::Selectable(choice.label.c_str(), selected) &&
                    PreviewSerializedProperty(property, selectedValue, "Edit " + property.displayName))
                {
                    changed = true;
                    CommitSerializedEdit();
                }
                ImGui::EndDisabled();
                DrawTooltipIfHovered(choice.tooltip);
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        break;
    }
    case InspectorPropertyKind::Enum:
    case InspectorPropertyKind::Flags:
    {
        const auto enumType = property.valueType.GetEnum();
        if (!enumType.IsValid())
        {
            ImGui::TextDisabled("Unavailable enum");
            break;
        }
        const int currentValue = value.ToInt();
        std::string currentLabel = "<Unknown>";
        for (const auto& key : enumType.GetKeys())
        {
            const auto candidate = enumType.GetValue(key);
            if (candidate.IsValid() && candidate.ToInt() == currentValue)
            {
                currentLabel = FormatInspectorDisplayName(key);
                break;
            }
        }
        if (property.kind == InspectorPropertyKind::Flags && currentLabel == "<Unknown>")
        {
            currentLabel.clear();
            for (const auto& key : enumType.GetKeys())
            {
                const auto candidate = enumType.GetValue(key);
                const int flag = candidate.IsValid() ? candidate.ToInt() : 0;
                if (flag != 0 && (currentValue & flag) == flag)
                {
                    if (!currentLabel.empty())
                        currentLabel += ", ";
                    currentLabel += FormatInspectorDisplayName(key);
                }
            }
            if (currentLabel.empty())
                currentLabel = currentValue == 0 ? "None" : "<Unknown flags>";
        }
        if (ImGui::BeginCombo(id.c_str(), currentLabel.c_str()))
        {
            for (const auto& key : enumType.GetKeys())
            {
                const auto candidate = enumType.GetValue(key);
                if (!candidate.IsValid())
                    continue;
                const int candidateValue = candidate.ToInt();
                const bool selected = property.kind == InspectorPropertyKind::Flags
                    ? (candidateValue == 0 ? currentValue == 0 : (currentValue & candidateValue) == candidateValue)
                    : candidateValue == currentValue;
                const auto selectableFlags = property.kind == InspectorPropertyKind::Flags
                    ? ImGuiSelectableFlags_DontClosePopups
                    : ImGuiSelectableFlags_None;
                if (ImGui::Selectable(FormatInspectorDisplayName(key).c_str(), selected, selectableFlags))
                {
                    auto selectedValue = candidate;
                    if (property.kind == InspectorPropertyKind::Flags)
                    {
                        const int requestedValue = candidateValue == 0
                            ? 0
                            : (selected ? currentValue & ~candidateValue : currentValue | candidateValue);
                        for (const auto& candidateKey : enumType.GetKeys())
                        {
                            const auto requested = enumType.GetValue(candidateKey);
                            if (requested.IsValid() && requested.ToInt() == requestedValue)
                            {
                                selectedValue = requested;
                                break;
                            }
                        }
                        if (selectedValue.ToInt() != requestedValue)
                        {
                            m_targetActionError = "The selected flag combination is not declared by this enum.";
                            continue;
                        }
                    }
                    if (PreviewSerializedProperty(property, selectedValue, "Edit " + property.displayName))
                    {
                        changed = true;
                        CommitSerializedEdit();
                    }
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        break;
    }
    case InspectorPropertyKind::Vector2:
    {
        auto edited = value.GetValue<Maths::Vector2>();
        float values[] = {edited.x, edited.y};
        changed = DrawLabeledFloatComponents(
            id,
            values,
            2u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale(),
            &componentEditCompleted);
        if (changed) PreviewValue(property, Maths::Vector2(values[0], values[1]), "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::Vector3:
    {
        auto edited = value.GetValue<Maths::Vector3>();
        float values[] = {edited.x, edited.y, edited.z};
        changed = DrawLabeledFloatComponents(
            id,
            values,
            3u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale(),
            &componentEditCompleted);
        if (changed) PreviewValue(property, Maths::Vector3(values[0], values[1], values[2]), "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::Vector4:
    {
        const auto edited = value.GetValue<Maths::Vector4>();
        float values[] = {edited.x, edited.y, edited.z, edited.w};
        changed = DrawLabeledFloatComponents(
            id,
            values,
            4u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale(),
            &componentEditCompleted);
        if (changed) PreviewValue(property, Maths::Vector4(values[0], values[1], values[2], values[3]), "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::Quaternion:
    {
        const auto euler = Maths::Quaternion::EulerAngles(value.GetValue<Maths::Quaternion>());
        float values[] = {euler.x, euler.y, euler.z};
        changed = DrawLabeledFloatComponents(
            id,
            values,
            3u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale(),
            &componentEditCompleted);
        if (changed)
            PreviewValue(property, Maths::Quaternion(Maths::Vector3(values[0], values[1], values[2])), "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::Color:
    {
        auto edited = value.GetValue<Maths::Color>();
        float values[] = {edited.r, edited.g, edited.b, edited.a};
        changed = DrawLabeledColorComponents(id, values, GetUiScale(), &componentEditCompleted);
        if (changed) PreviewValue(property, Maths::Color(values[0], values[1], values[2], values[3]), "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::Rect:
    {
        auto edited = value.GetValue<Maths::Rect>();
        float values[] = {edited.x, edited.y, edited.width, edited.height};
        changed = DrawLabeledFloatComponents(
            id,
            values,
            4u,
            kInspectorRectComponentLabels.data(),
            0.01f,
            GetUiScale(),
            &componentEditCompleted);
        if (changed) PreviewValue(property, Maths::Rect(values[0], values[1], values[2], values[3]), "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::Bounds:
    case InspectorPropertyKind::BoundingSphere:
    case InspectorPropertyKind::Struct:
    {
        changed = DrawLooseValue(value, property.valueType, id);
        if (changed) PreviewSerializedProperty(property, value, "Edit " + property.displayName);
        break;
    }
    case InspectorPropertyKind::LayerMask:
    {
        auto edited = value.GetValue<Engine::LayerMask>();
        uint32_t mask = edited.GetMask();
        changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_U32, &mask);
        if (changed)
        {
            edited.SetMask(mask);
            PreviewValue(property, edited, "Edit " + property.displayName);
        }
        break;
    }
    case InspectorPropertyKind::Array:
    {
        changed = DrawArrayValue(value, property.valueType, id, 0);
        if (changed)
        {
            PreviewSerializedProperty(property, value, "Edit " + property.displayName);
            CommitSerializedEdit();
            m_treeDirty = true;
            StructureChangedEvent.Invoke();
        }
        break;
    }
    case InspectorPropertyKind::ObjectReference:
    {
        changed = DrawObjectReferenceValue(property, value, id);
        break;
    }
    case InspectorPropertyKind::Unsupported:
    default:
        ImGui::TextDisabled("Read only");
        break;
    }

    ImGui::EndDisabled();
    if (componentEditCompleted && !ImGui::IsKeyPressed(ImGuiKey_Escape))
        CommitSerializedEdit();
    else
        CommitOrCancelAfterItem(property);
    ImGui::PopID();
    return changed;
}

bool InspectorView::DrawObjectReferenceValue(
    const PropertyNode& property,
    meta::Variant& value,
    const std::string& id)
{
    const std::string display = GetInspectorObjectReferenceDisplay(value, property.valueType);
    const auto commit = [this, &property](meta::Variant updated, const char* action)
    {
        if (PreviewSerializedProperty(property, std::move(updated), action))
        {
            CommitSerializedEdit();
            return true;
        }
        return false;
    };

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
            return AssignInspectorAssetReference(updated, referenceType, payload) &&
                commit(std::move(updated), "Assign Object Reference");
        };
    }
    if (const auto pingTarget = GetInspectorObjectReferencePingTarget(value, referenceType); pingTarget.has_value())
    {
        picker.ping = [target = *pingTarget]
        {
            EditorSelectionService::Get().Select(target);
        };
    }
    picker.clear = [&value, referenceType, commit]
    {
        auto updated = value;
        return ClearInspectorObjectReference(updated, referenceType) &&
            commit(std::move(updated), "Clear Object Reference");
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
                items.push_back({
                    entry.displayName,
                    entry.displayName,
                    [&value, referenceType, payload, commit]
                    {
                        auto updated = value;
                        return AssignInspectorAssetReference(updated, referenceType, payload) &&
                            commit(std::move(updated), "Assign Object Reference");
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
                items.push_back({
                    entry.displayName,
                    entry.displayName,
                    [&value, referenceType, gameObject, component, commit]
                    {
                        auto updated = value;
                        return AssignInspectorSceneReference(
                            updated,
                            referenceType,
                            gameObject,
                            component) &&
                            commit(std::move(updated), "Assign Object Reference");
                    }});
            }
        }
        return items;
    };
    return DrawInspectorObjectReferencePicker(m_objectReferencePickerState, picker);
}

bool InspectorView::DrawLooseValue(meta::Variant& value, const meta::Type& type, const std::string& id, const int depth)
{
    if (depth > kMaximumNestedDepth || !value.IsValid())
    {
        ImGui::TextDisabled("Maximum nested depth reached");
        return false;
    }

    const auto kind = GetInspectorPropertyKind(type);
    bool changed = false;
    switch (kind)
    {
    case InspectorPropertyKind::Boolean:
    {
        bool edited = value.GetValue<bool>();
        changed = ImGui::Checkbox(id.c_str(), &edited);
        if (changed) value = edited;
        break;
    }
    case InspectorPropertyKind::SignedInteger8:
    {
        int8_t edited = value.GetValue<int8_t>(); changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_S8, &edited); if (changed) value = edited; break;
    }
    case InspectorPropertyKind::UnsignedInteger8:
    {
        uint8_t edited = value.GetValue<uint8_t>(); changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_U8, &edited); if (changed) value = edited; break;
    }
    case InspectorPropertyKind::SignedInteger16:
    {
        int16_t edited = value.GetValue<int16_t>(); changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_S16, &edited); if (changed) value = edited; break;
    }
    case InspectorPropertyKind::UnsignedInteger16:
    {
        uint16_t edited = value.GetValue<uint16_t>(); changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_U16, &edited); if (changed) value = edited; break;
    }
    case InspectorPropertyKind::SignedInteger32:
    {
        int32_t edited = value.GetValue<int32_t>(); changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_S32, &edited); if (changed) value = edited; break;
    }
    case InspectorPropertyKind::UnsignedInteger32:
    {
        uint32_t edited = value.GetValue<uint32_t>(); changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_U32, &edited); if (changed) value = edited; break;
    }
    case InspectorPropertyKind::SignedInteger64:
    {
        int64_t edited = value.GetValue<int64_t>(); changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_S64, &edited); if (changed) value = edited; break;
    }
    case InspectorPropertyKind::UnsignedInteger64:
    {
        uint64_t edited = value.GetValue<uint64_t>(); changed = ImGui::InputScalar(id.c_str(), ImGuiDataType_U64, &edited); if (changed) value = edited; break;
    }
    case InspectorPropertyKind::Float:
    {
        float edited = value.GetValue<float>(); changed = ImGui::DragFloat(id.c_str(), &edited, 0.01f); if (changed) value = edited; break;
    }
    case InspectorPropertyKind::Double:
    {
        double edited = value.GetValue<double>(); changed = ImGui::InputDouble(id.c_str(), &edited); if (changed) value = edited; break;
    }
    case InspectorPropertyKind::String:
    {
        std::vector<char> text; CopyToBuffer(value.GetValue<std::string>(), text, 256u); changed = ImGui::InputText(id.c_str(), text.data(), text.size()); if (changed) value = std::string(text.data()); break;
    }
    case InspectorPropertyKind::Vector2:
    {
        auto edited = value.GetValue<Maths::Vector2>();
        float values[] = {edited.x, edited.y};
        changed = DrawLabeledFloatComponents(
            id,
            values,
            2u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale());
        if (changed) value = Maths::Vector2(values[0], values[1]);
        break;
    }
    case InspectorPropertyKind::Vector3:
    {
        auto edited = value.GetValue<Maths::Vector3>();
        float values[] = {edited.x, edited.y, edited.z};
        changed = DrawLabeledFloatComponents(
            id,
            values,
            3u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale());
        if (changed) value = Maths::Vector3(values[0], values[1], values[2]);
        break;
    }
    case InspectorPropertyKind::Vector4:
    {
        auto edited = value.GetValue<Maths::Vector4>();
        float values[] = {edited.x, edited.y, edited.z, edited.w};
        changed = DrawLabeledFloatComponents(
            id,
            values,
            4u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale());
        if (changed) value = Maths::Vector4(values[0], values[1], values[2], values[3]);
        break;
    }
    case InspectorPropertyKind::Quaternion:
    {
        const auto euler = Maths::Quaternion::EulerAngles(value.GetValue<Maths::Quaternion>());
        float values[] = {euler.x, euler.y, euler.z};
        changed = DrawLabeledFloatComponents(
            id,
            values,
            3u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale());
        if (changed) value = Maths::Quaternion(Maths::Vector3(values[0], values[1], values[2]));
        break;
    }
    case InspectorPropertyKind::Color:
    {
        auto edited = value.GetValue<Maths::Color>();
        float values[] = {edited.r, edited.g, edited.b, edited.a};
        changed = DrawLabeledColorComponents(id, values, GetUiScale());
        if (changed) value = Maths::Color(values[0], values[1], values[2], values[3]);
        break;
    }
    case InspectorPropertyKind::Rect:
    {
        auto edited = value.GetValue<Maths::Rect>();
        float values[] = {edited.x, edited.y, edited.width, edited.height};
        changed = DrawLabeledFloatComponents(
            id,
            values,
            4u,
            kInspectorRectComponentLabels.data(),
            0.01f,
            GetUiScale());
        if (changed) value = Maths::Rect(values[0], values[1], values[2], values[3]);
        break;
    }
    case InspectorPropertyKind::Bounds:
    {
        auto edited = value.GetValue<Render::Geometry::Bounds>();
        float center[] = {edited.center.x, edited.center.y, edited.center.z};
        float size[] = {edited.size.x, edited.size.y, edited.size.z};
        ImGui::TextUnformatted("Center");
        changed = DrawLabeledFloatComponents(
            id + ".center",
            center,
            3u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale());
        ImGui::TextUnformatted("Size");
        changed = DrawLabeledFloatComponents(
            id + ".size",
            size,
            3u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale()) || changed;
        if (changed) value = Render::Geometry::Bounds { Maths::Vector3(center[0], center[1], center[2]), Maths::Vector3(size[0], size[1], size[2]) };
        break;
    }
    case InspectorPropertyKind::BoundingSphere:
    {
        auto edited = value.GetValue<Render::Geometry::BoundingSphere>();
        float position[] = {edited.position.x, edited.position.y, edited.position.z};
        ImGui::TextUnformatted("Position");
        changed = DrawLabeledFloatComponents(
            id + ".position",
            position,
            3u,
            kInspectorVectorComponentLabels.data(),
            0.01f,
            GetUiScale());
        ImGui::TextUnformatted("Radius");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed = ImGui::DragFloat((id + ".radius").c_str(), &edited.radius, 0.01f) || changed;
        if (changed)
        {
            edited.position = Maths::Vector3(position[0], position[1], position[2]);
            value = edited;
        }
        break;
    }
    case InspectorPropertyKind::Array:
        changed = DrawArrayValue(value, type, id, depth + 1);
        break;
    case InspectorPropertyKind::Struct:
        changed = DrawStructValue(value, type, id, depth + 1);
        break;
    case InspectorPropertyKind::ObjectReference:
    {
        const auto display = GetInspectorObjectReferenceDisplay(value, type);
        ImGui::TextUnformatted(TruncateUtf8ForWidth(display, ImGui::GetContentRegionAvail().x - 34.0f * GetUiScale()).c_str());
        DrawTooltipIfHovered(display);
        ImGui::SameLine();
        if (ImGui::SmallButton("X") && ClearInspectorObjectReference(value, type))
            changed = true;
        DrawTooltipIfHovered("Clear reference");
        break;
    }
    default:
        ImGui::TextDisabled("Read only");
        break;
    }
    return changed;
}

bool InspectorView::DrawArrayValue(meta::Variant& value, const meta::Type& arrayType, const std::string& id, const int depth)
{
    if (depth > kMaximumNestedDepth || !arrayType.IsArray())
        return false;

    auto array = value.GetArray();
    if (!array.IsValid())
    {
        ImGui::TextDisabled("Array unavailable");
        return false;
    }

    bool changed = false;
    const auto elementType = arrayType.GetArrayType();
    const auto elementKind = GetInspectorPropertyKind(elementType);
    const std::string label = "Array (" + std::to_string(array.Size()) + ")";
    const bool opened = ImGui::TreeNodeEx((id + ".foldout").c_str(), ImGuiTreeNodeFlags_DefaultOpen, "%s", label.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("+"))
    {
        if (array.CanInsertDefault())
        {
            array.InsertDefault(array.Size());
            changed = true;
        }
    }
    DrawTooltipIfHovered("Append element");
    ImGui::SameLine();
    ImGui::BeginDisabled(!IsSimpleArraySortType(elementKind));
    if (ImGui::SmallButton("Sort"))
    {
        std::vector<meta::Variant> values;
        values.reserve(array.Size());
        for (size_t index = 0u; index < array.Size(); ++index)
            values.push_back(array.GetValue(index));
        std::sort(values.begin(), values.end(), [&](const meta::Variant& left, const meta::Variant& right)
        {
            return CompareArrayValues(left, right, elementKind);
        });
        for (size_t index = 0u; index < values.size(); ++index)
            array.SetValue(index, values[index]);
        changed = true;
    }
    ImGui::EndDisabled();

    if (!opened)
        return changed;

    int requestedSize = static_cast<int>((std::min)(array.Size(), static_cast<size_t>(kMaximumArraySize)));
    ImGui::SetNextItemWidth(92.0f * GetUiScale());
    if (ImGui::InputInt((id + ".size").c_str(), &requestedSize))
    {
        requestedSize = std::clamp(requestedSize, 0, kMaximumArraySize);
        changed = ResizeArray(array, static_cast<size_t>(requestedSize)) || changed;
    }

    for (size_t index = 0u; index < array.Size(); ++index)
    {
        ImGui::PushID(static_cast<int>(index));
        auto element = array.GetValue(index);
        ImGui::Text("[%zu]", index);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool edited = DrawLooseValue(element, elementType, id + "." + std::to_string(index), depth + 1);
        if (edited && array.CanSetValue())
        {
            array.SetValue(index, element);
            changed = true;
        }

        if (ImGui::SmallButton("Copy"))
        {
            m_arrayClipboard = element;
            m_arrayClipboardType = elementType.GetName();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_arrayClipboard || m_arrayClipboardType != elementType.GetName() || !array.CanSetValue());
        if (ImGui::SmallButton("Paste"))
        {
            array.SetValue(index, *m_arrayClipboard);
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(index == 0u || !array.CanSetValue());
        if (ImGui::SmallButton("Up"))
        {
            const auto previous = array.GetValue(index - 1u);
            array.SetValue(index - 1u, element);
            array.SetValue(index, previous);
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(index + 1u >= array.Size() || !array.CanSetValue());
        if (ImGui::SmallButton("Down"))
        {
            const auto next = array.GetValue(index + 1u);
            array.SetValue(index + 1u, element);
            array.SetValue(index, next);
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!array.CanRemove());
        if (ImGui::SmallButton("X"))
        {
            array.Remove(index);
            changed = true;
            ImGui::EndDisabled();
            ImGui::PopID();
            break;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::TreePop();
    return changed;
}

bool InspectorView::DrawStructValue(meta::Variant& value, const meta::Type& type, const std::string& id, const int depth)
{
    if (depth > kMaximumNestedDepth)
        return false;

    bool changed = false;
    const bool opened = ImGui::TreeNodeEx((id + ".struct").c_str(), ImGuiTreeNodeFlags_DefaultOpen, "%s", FormatInspectorDisplayName(type.GetName()).c_str());
    if (!opened)
        return false;

    if (ImGui::BeginTable((id + ".structFields").c_str(), 2,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV))
    {
        const float availableLogicalWidth = ImGui::GetContentRegionAvail().x / GetUiScale();
        const float labelWidth = std::clamp(
            availableLogicalWidth * 0.34f,
            kInspectorMinimumLabelLogicalWidth,
            kInspectorMaximumLabelLogicalWidth);
        ImGui::TableSetupColumn("##InspectorStructLabel", ImGuiTableColumnFlags_WidthFixed, labelWidth * GetUiScale());
        ImGui::TableSetupColumn("##InspectorStructValue", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& field : type.GetFields())
        {
            if (!field.IsValid() || field.IsReadOnly())
                continue;
            auto child = field.GetValue(value);
            if (!child.IsValid())
                continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const auto fieldLabel = FormatInspectorDisplayName(field.GetName());
            const auto visibleLabel = TruncateUtf8ForWidth(fieldLabel, ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(visibleLabel.c_str());
            DrawTooltipIfHovered(visibleLabel == fieldLabel ? std::string {} : fieldLabel);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (DrawLooseValue(child, field.GetType(), id + "." + field.GetName(), depth + 1) && field.SetValue(value, child))
                changed = true;
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
    return changed;
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
        // The override is scoped to one synchronous PropertyField draw. Its
        // public stream path is component-relative while the UI projection path
        // is GameObject-qualified, so identity comes from the scope rather than
        // comparing two different path roots.
        return m_serializedDrawOverride->property;
    }
    if (!m_serializedObject || property.path.Empty())
        return {};
    const auto path = property.path.ToString();
    if (auto exact = m_serializedObject->FindProperty(path); exact)
        return exact;

    // Projection paths can be GameObject/component-qualified while the
    // canonical TypeTree keeps component-local external editor property paths.  Resolve
    // a suffix from the stream instead of reviving a callback binding.
    for (size_t separator = path.find('.'); separator != std::string::npos;
         separator = path.find('.', separator + 1u))
    {
        if (auto relative = m_serializedObject->FindProperty(path.substr(separator + 1u)); relative)
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
    auto* serializedOwner = m_serializedDrawOverride.has_value()
        ? m_serializedDrawOverride->owner
        : m_serializedObject.get();
    return serializedOwner != nullptr && serializedOwner->HasModifiedProperties() &&
        serializedOwner->ApplyModifiedProperties();
}

void InspectorView::CommitOrCancelAfterItem(const PropertyNode& property)
{
    auto* serializedOwner = m_serializedDrawOverride.has_value()
        ? m_serializedDrawOverride->owner
        : m_serializedObject.get();
    if (serializedOwner == nullptr || !serializedOwner->HasModifiedProperties())
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        serializedOwner->DiscardModifiedProperties();
        NotifyPropertyChanged(property);
        return;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        static_cast<void>(CommitSerializedEdit());
}

void InspectorView::NotifyPropertyChanged(const PropertyNode& property)
{
    m_prefabOverridesDirty = true;
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

template <typename TValue>
bool InspectorView::PreviewValue(const PropertyNode& property, const TValue& value, std::string actionLabel)
{
    TValue copiedValue = value;
    meta::Variant updated(copiedValue);
    if (!PreviewSerializedProperty(property, std::move(updated), std::move(actionLabel)))
        return false;
    return true;
}

template bool InspectorView::PreviewValue<bool>(const PropertyNode&, const bool&, std::string);
template bool InspectorView::PreviewValue<int8_t>(const PropertyNode&, const int8_t&, std::string);
template bool InspectorView::PreviewValue<uint8_t>(const PropertyNode&, const uint8_t&, std::string);
template bool InspectorView::PreviewValue<int16_t>(const PropertyNode&, const int16_t&, std::string);
template bool InspectorView::PreviewValue<uint16_t>(const PropertyNode&, const uint16_t&, std::string);
template bool InspectorView::PreviewValue<int32_t>(const PropertyNode&, const int32_t&, std::string);
template bool InspectorView::PreviewValue<uint32_t>(const PropertyNode&, const uint32_t&, std::string);
template bool InspectorView::PreviewValue<int64_t>(const PropertyNode&, const int64_t&, std::string);
template bool InspectorView::PreviewValue<uint64_t>(const PropertyNode&, const uint64_t&, std::string);
template bool InspectorView::PreviewValue<float>(const PropertyNode&, const float&, std::string);
template bool InspectorView::PreviewValue<double>(const PropertyNode&, const double&, std::string);
template bool InspectorView::PreviewValue<std::string>(const PropertyNode&, const std::string&, std::string);
template bool InspectorView::PreviewValue<Maths::Vector2>(const PropertyNode&, const Maths::Vector2&, std::string);
template bool InspectorView::PreviewValue<Maths::Vector3>(const PropertyNode&, const Maths::Vector3&, std::string);
template bool InspectorView::PreviewValue<Maths::Vector4>(const PropertyNode&, const Maths::Vector4&, std::string);
template bool InspectorView::PreviewValue<Maths::Quaternion>(const PropertyNode&, const Maths::Quaternion&, std::string);
template bool InspectorView::PreviewValue<Maths::Color>(const PropertyNode&, const Maths::Color&, std::string);
template bool InspectorView::PreviewValue<Maths::Rect>(const PropertyNode&, const Maths::Rect&, std::string);
template bool InspectorView::PreviewValue<Engine::LayerMask>(const PropertyNode&, const Engine::LayerMask&, std::string);
} // namespace NLS::Editor::Inspector
