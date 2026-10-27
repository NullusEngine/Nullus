#include "Properties/CollectionPropertyDrawer.h"

#include "Inspector/PropertyTypeTraits.h"
#include "Inspector/ObjectReferenceSupport.h"
#include "Inspector/EditorSelectionService.h"
#include "Inspector/NativeReflectionAdapter.h"
#include "Assets/EditorAssetDragPayload.h"
#include <GameObject.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Layout/TreeNode.h>
#include <imgui_internal.h>
#include <algorithm>
#include <numeric>

namespace NLS::Editor::Properties
{
using namespace Inspector;
using namespace Serialization;
using namespace UI::Controls;

CollectionClipboard& GetCollectionClipboard()
{
    static CollectionClipboard clipboard;
    return clipboard;
}

PropertyNode MakeCollectionElementNode(const PropertyNode& parent, const SerializedProperty& element, size_t index)
{
    PropertyNode node;
    node.path = PropertyPath(std::string(element.GetPropertyPath()));
    node.fieldName = parent.fieldName;
    node.ownerTypeName = parent.ownerTypeName;
    node.displayName = "Element " + std::to_string(index);
    node.valueType = meta::Type(static_cast<meta::TypeID>(element.GetExactTypeId()), element.IsArray());
    if (!node.valueType.IsValid() && parent.valueType.IsValid())
        node.valueType = parent.valueType.GetArrayType();
    if (!node.valueType.IsValid())
        node.valueType = element.GetValue().GetType();
    node.kind = element.IsArray() ? InspectorPropertyKind::Array : GetInspectorPropertyKind(node.valueType);
    node.metadata = parent.metadata;
    node.metadata.header.clear();
    node.metadata.spaceBefore = 0;
    node.metadata.visibleIf.clear();
    node.metadata.enabledIf.clear();
    if (node.metadata.flags && node.kind == InspectorPropertyKind::Enum)
        node.kind = InspectorPropertyKind::Flags;
    node.choices = parent.choices;
    node.target = parent.target;
    node.targets = parent.targets;
    node.readOnly = parent.readOnly || element.IsReadOnly();
    node.isFixedBuffer = element.IsFixedBuffer();
    return node;
}

CollectionCommandResult ExecuteCollectionCommand(SerializedObject& object, const PropertyNode& property,
                                                 const CollectionCommand& command, CollectionClipboard& clipboard)
{
    CollectionCommandResult result;
    auto array = object.FindProperty(property.path.ToString());
    if (!array || !array.IsArray())
    {
        result.error = "Array is no longer available.";
        return result;
    }
    const auto count = array.GetMinArraySize();
    result.count = count;
    result.revision = object.GetVersion().structureGeneration;
    auto selected = command.indices;
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    if (std::any_of(selected.begin(), selected.end(), [=](size_t i)
                    { return i >= count; }))
    {
        result.error = "Array selection is no longer available.";
        return result;
    }
    result.selection = selected;
    const bool copy = command.action == CollectionAction::Copy || command.action == CollectionAction::CopyCollection;
    const bool expand = command.action == CollectionAction::ExpandAll || command.action == CollectionAction::CollapseAll;
    if (!copy && !expand && (!array.IsEditable() || property.readOnly || property.metadata.readOnly))
        return result;
    const bool valuePaste = command.action == CollectionAction::Paste;
    if (!copy && !expand && !valuePaste && array.IsFixedBuffer())
        return result;

    switch (command.action)
    {
        case CollectionAction::Add:
            // Unity's default ReorderableList adds through arraySize = count + 1.
            result.changed = array.SetArraySize(count + 1);
            result.selection = {count};
            break;
        case CollectionAction::Resize:
            result.changed = array.SetArraySize(command.size);
            std::erase_if(result.selection, [&](size_t i)
                          { return i >= command.size; });
            break;
        case CollectionAction::Clear:
            result.changed = array.ClearArray();
            result.selection.clear();
            break;
        case CollectionAction::Remove:
            if (selected.empty() && count)
                selected.push_back(count - 1);
            // DeleteArrayElementAtIndex retains its native two-step reference
            // semantics. A list delete command removes the selected slots once.
            result.changed = array.RemoveArrayElements(selected);
            result.selection.clear();
            if (count > selected.size() && !selected.empty())
                result.selection = {(std::min)(selected.front() ? selected.front() - 1 : 0, count - selected.size() - 1)};
            break;
        case CollectionAction::Duplicate:
            result.changed = array.DuplicateArrayElements(selected);
            result.selection.clear();
            for (size_t i = 0; i < selected.size(); ++i)
                result.selection.push_back(selected[i] + i + 1);
            break;
        case CollectionAction::Move:
        {
            const auto order = CollectionMoveOrder(count, selected, command.insertion);
            if (order.size() != count)
                break;
            result.changed = array.SetArrayOrder(order);
            result.selection.clear();
            for (size_t i = 0; i < order.size(); ++i)
                if (std::binary_search(selected.begin(), selected.end(), order[i]))
                    result.selection.push_back(i);
            break;
        }
        case CollectionAction::CopyCollection:
            clipboard.value = SerializedValue::Capture(array.GetValue());
            clipboard.elements.clear();
            break;
        case CollectionAction::Copy:
        {
            std::vector<SerializedValue> values;
            for (const auto i : selected)
            {
                auto value = SerializedValue::Capture(array.GetArrayElementAtIndex(i).GetValue());
                if (!value)
                {
                    result.error = "The element cannot be copied.";
                    return result;
                }
                values.push_back(std::move(*value));
            }
            clipboard.elements = std::move(values);
            if (clipboard.elements.size() == 1)
                clipboard.value = clipboard.elements.front();
            break;
        }
        case CollectionAction::PasteCollection:
            if (clipboard.value)
                if (auto value = clipboard.value->Resolve(property.valueType))
                    result.changed = array.SetValue(*value);
            break;
        case CollectionAction::Paste:
        {
            const auto elementType = count ? array.GetArrayElementAtIndex(0).GetValue().GetType() : property.valueType.GetArrayType();
            std::vector<meta::Variant> values;
            const auto& snapshots = clipboard.elements;
            if (snapshots.empty() && clipboard.value)
            {
                if (auto value = clipboard.value->Resolve(elementType))
                    values.push_back(std::move(*value));
            }
            else
                for (const auto& snapshot : snapshots)
                {
                    auto value = snapshot.Resolve(elementType);
                    if (!value)
                    {
                        result.error = "Clipboard element types do not match.";
                        return result;
                    }
                    values.push_back(std::move(*value));
                }
            if (values.empty() || selected.empty() || (values.size() != 1 && values.size() != selected.size()))
                break;
            for (size_t i = 0; i < selected.size(); ++i)
                result.changed |= array.GetArrayElementAtIndex(selected[i]).SetValue(values[values.size() == 1 ? 0 : i]);
            break;
        }
        case CollectionAction::ExpandAll:
        case CollectionAction::CollapseAll:
        {
            const bool expanded = command.action == CollectionAction::ExpandAll;
            if (selected.empty())
                for (size_t i = 0; i < count; ++i)
                    selected.push_back(i);
            for (const auto i : selected)
                array.GetArrayElementAtIndex(i).SetExpanded(expanded);
            break;
        }
    }
    result.count = object.FindProperty(property.path.ToString()).GetMinArraySize();
    result.revision = object.GetVersion().structureGeneration;
    if (!result.changed && !object.GetLastError().empty())
        result.error = object.GetLastError();
    return result;
}

bool DrawCollectionProperty(PropertyDrawerContext& context)
{
    if (!context.serializedProperty || !context.serializedObject || !context.drawChild)
        return false;
    auto& array = *context.serializedProperty;
    struct BindingState
    {
        CollectionState control;
        bool initialized = false, observedExpanded = false;
    };
    auto& binding = context.GetState<BindingState>();
    auto& state = binding.control;
    if (!binding.initialized || binding.observedExpanded != array.IsExpanded())
    {
        state.expanded = array.IsExpanded();
        binding.observedExpanded = state.expanded;
        binding.initialized = true;
    }
    if (context.expandedOverride)
        state.expanded = *context.expandedOverride;
    CollectionOptions options;
    options.label = context.property.displayName;
    options.tooltip = context.property.metadata.tooltip;
    const auto sizes = array.GetArraySizes();
    options.primaryCount = array.GetArraySize();
    options.count = sizes.empty() ? 0 : *std::min_element(sizes.begin(), sizes.end());
    options.mixedSize = std::any_of(sizes.begin(), sizes.end(), [&](size_t n)
                                    { return n != options.primaryCount; });
    options.revision = context.serializedObject->GetVersion().structureGeneration;
    options.dataRevision = context.serializedObject->GetVersion().dataGeneration;
    options.fixedSize = array.IsFixedBuffer();
    options.readOnly = context.readOnly;
    options.scale = context.uiScale;
    options.elementLabelWidth = (std::max)(20.f, context.labelColumnWidth / (std::max)(context.uiScale, .1f) - 20.f);
    options.multiEditLimit = context.serializedObject->GetMaxArraySizeForMultiEditing();
    options.overMultiEditLimit = array.GetTargetCount() > 1 && options.count > options.multiEditLimit;
    if (options.overMultiEditLimit)
        options.count = 0;
    CollectionCallbacks callbacks;
    callbacks.composite = [&](size_t i)
    {
        const auto node = MakeCollectionElementNode(context.property, array.GetArrayElementAtIndex(i), i);
        return node.kind == InspectorPropertyKind::Array || node.kind == InspectorPropertyKind::Struct;
    };
    callbacks.drawElement = [&](size_t i, float width)
    {
        auto node = MakeCollectionElementNode(context.property, array.GetArrayElementAtIndex(i), i);
        // Nested controls measure their width inside this row, including any
        // further arrays; no child can escape into the inspector label column.
        auto* window = ImGui::GetCurrentWindow();
        const auto work = window->WorkRect;
        const auto content = window->ContentRegionRect;
        window->WorkRect.Max.x = window->ContentRegionRect.Max.x = ImGui::GetCursorScreenPos().x + width;
        auto result = context.drawChild(node);
        window->WorkRect = work;
        window->ContentRegionRect = content;
        return result;
    };
    callbacks.execute = [&](const CollectionCommand& command)
    {
        auto result = ExecuteCollectionCommand(*context.serializedObject, context.property, command, GetCollectionClipboard());
        if (context.setExpanded && (command.action == CollectionAction::ExpandAll || command.action == CollectionAction::CollapseAll))
        {
            auto indices = command.indices;
            if (indices.empty())
            {
                indices.resize(options.count);
                std::iota(indices.begin(), indices.end(), 0);
            }
            for (auto i : indices)
                context.setExpanded(array.GetArrayElementAtIndex(i).GetPropertyPath(), command.action == CollectionAction::ExpandAll);
        }
        if (result.changed)
        {
            context.structureChanged |= command.action != CollectionAction::Paste;
            if (context.notifyChanged)
                context.notifyChanged(context.property);
        }
        return result;
    };
    callbacks.available = [&](CollectionAction action)
    {
        auto& clipboard = GetCollectionClipboard();
        if (action == CollectionAction::PasteCollection)
            return clipboard.value && clipboard.value->Resolve(context.property.valueType).has_value();
        if (action == CollectionAction::Paste)
        {
            const auto type = context.property.valueType.GetArrayType();
            if (!clipboard.elements.empty())
                return clipboard.elements.front().Resolve(type).has_value();
            return clipboard.value && clipboard.value->Resolve(type).has_value();
        }
        return true;
    };
    callbacks.drawMenu = context.drawMenu;
    callbacks.drop = [&]
    {
        CollectionCommandResult result;
        const auto type = context.property.valueType.GetArrayType();
        const auto* payload = ImGui::GetDragDropPayload();
        if (!payload)
            return result;
        std::vector<meta::Variant> values;
        if (payload->IsDataType(Assets::kEditorAssetDragPayloadType) || payload->IsDataType(Assets::kEditorAssetsDragPayloadType))
        {
            if (payload->DataSize <= 0 || payload->DataSize % sizeof(Assets::EditorAssetDragPayload))
                return result;
            const auto* assets = static_cast<const Assets::EditorAssetDragPayload*>(payload->Data);
            for (size_t i = 0; i < static_cast<size_t>(payload->DataSize) / sizeof(*assets); ++i)
            {
                if (!IsCompatibleInspectorReferenceArtifact(type, Assets::GetEditorAssetDragPayloadArtifactType(assets[i])))
                    continue;
                if (const auto* accepted = ImGui::AcceptDragDropPayload(payload->DataType); accepted && accepted->IsDelivery())
                {
                    meta::Variant value;
                    if (AssignInspectorAssetReference(value, type, assets[i]))
                        values.push_back(std::move(value));
                }
            }
        }
        else if (payload->IsDataType("GameObject") && IsInspectorSceneObjectReferenceType(type) && payload->DataSize == sizeof(std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>))
        {
            auto* dragged = static_cast<const std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>*>(payload->Data)->first;
            if (!dragged)
                return result;
            std::vector<Engine::GameObject*> objects;
            for (const auto& handle : EditorSelectionService::Get().GetSelection().targets)
                if (auto* object = dynamic_cast<Engine::GameObject*>(NativeReflectionAdapter::ResolveObject(handle)))
                    objects.push_back(object);
            // Resolve the current durable selection before touching a legacy
            // drag pointer. A source destroyed during the drag is rejected.
            if (std::find(objects.begin(), objects.end(), dragged) == objects.end())
                return result;
            for (auto* object : objects)
            {
                meta::Variant value;
                if (!AssignInspectorSceneReference(value, type, object))
                    continue;
                if (const auto* accepted = ImGui::AcceptDragDropPayload("GameObject"); accepted && accepted->IsDelivery())
                    values.push_back(std::move(value));
            }
        }
        if (!values.empty())
        {
            result.changed = array.AppendArrayValues(values);
            if (result.changed)
            {
                result.count = array.GetMinArraySize();
                result.revision = context.serializedObject->GetVersion().structureGeneration;
                const size_t start = result.count - values.size();
                for (size_t i = 0; i < values.size(); ++i)
                    result.selection.push_back(start + i);
                context.structureChanged = true;
                if (context.notifyChanged)
                    context.notifyChanged(context.property);
            }
            else
                result.error = context.serializedObject->GetLastError();
        }
        return result;
    };
    context.interaction = Collection(context.widgetId.c_str(), state, options, callbacks);
    context.changed = context.interaction.changed;
    return true;
}
} // namespace NLS::Editor::Properties
