#include "Inspector/InspectorProjectionBuilder.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "Inspector/NativeReflectionAdapter.h"
#include "Inspector/ObjectReferenceSupport.h"
#include "Inspector/PropertyTypeTraits.h"
#include "Inspector/ScriptSerializedAdapter.h"
#include "Serialization/SerializedObject.h"

#include <Scripting/ScriptComponent.h>

namespace NLS::Editor::Inspector
{
namespace
{
std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool MatchesSearch(const PropertyNode& node, const std::string_view searchText)
{
    if (searchText.empty())
        return true;

    const auto needle = LowerAscii(std::string(searchText));
    return LowerAscii(node.displayName).find(needle) != std::string::npos ||
        LowerAscii(node.fieldName).find(needle) != std::string::npos ||
        LowerAscii(node.path.ToString()).find(needle) != std::string::npos ||
        LowerAscii(node.metadata.tooltip).find(needle) != std::string::npos;
}

InspectorPropertyKind GetPropertyKind(const Serialization::SerializedProperty& property)
{
    if (property.IsArray())
        return InspectorPropertyKind::Array;

    switch (property.GetPropertyType())
    {
    case Serialization::SerializedPropertyType::Boolean: return InspectorPropertyKind::Boolean;
    case Serialization::SerializedPropertyType::Float:
        return property.GetNumericType() == Serialization::SerializedNumericType::Double
            ? InspectorPropertyKind::Double
            : InspectorPropertyKind::Float;
    case Serialization::SerializedPropertyType::String: return InspectorPropertyKind::String;
    case Serialization::SerializedPropertyType::Color: return InspectorPropertyKind::Color;
    case Serialization::SerializedPropertyType::ObjectReference:
    case Serialization::SerializedPropertyType::ExposedReference: return InspectorPropertyKind::ObjectReference;
    case Serialization::SerializedPropertyType::LayerMask: return InspectorPropertyKind::LayerMask;
    case Serialization::SerializedPropertyType::Enum: return InspectorPropertyKind::Enum;
    case Serialization::SerializedPropertyType::Vector2: return InspectorPropertyKind::Vector2;
    case Serialization::SerializedPropertyType::Vector3: return InspectorPropertyKind::Vector3;
    case Serialization::SerializedPropertyType::Vector4: return InspectorPropertyKind::Vector4;
    case Serialization::SerializedPropertyType::Quaternion: return InspectorPropertyKind::Quaternion;
    case Serialization::SerializedPropertyType::Rect: return InspectorPropertyKind::Rect;
    case Serialization::SerializedPropertyType::Bounds: return InspectorPropertyKind::Bounds;
    case Serialization::SerializedPropertyType::Integer:
    case Serialization::SerializedPropertyType::Character:
        switch (property.GetNumericType())
        {
        case Serialization::SerializedNumericType::Int8: return InspectorPropertyKind::SignedInteger8;
        case Serialization::SerializedNumericType::UInt8: return InspectorPropertyKind::UnsignedInteger8;
        case Serialization::SerializedNumericType::Int16: return InspectorPropertyKind::SignedInteger16;
        case Serialization::SerializedNumericType::UInt16: return InspectorPropertyKind::UnsignedInteger16;
        case Serialization::SerializedNumericType::Int32: return InspectorPropertyKind::SignedInteger32;
        case Serialization::SerializedNumericType::UInt32: return InspectorPropertyKind::UnsignedInteger32;
        case Serialization::SerializedNumericType::Int64: return InspectorPropertyKind::SignedInteger64;
        case Serialization::SerializedNumericType::UInt64: return InspectorPropertyKind::UnsignedInteger64;
        default: return InspectorPropertyKind::Unsupported;
        }
    case Serialization::SerializedPropertyType::Generic:
    case Serialization::SerializedPropertyType::FixedBufferSize:
    default: return property.HasChildren() ? InspectorPropertyKind::Struct : InspectorPropertyKind::Unsupported;
    }
}

PropertyNode BuildNode(
    const Serialization::SerializedProperty& property,
    const InspectorTargetSet& targets)
{
    PropertyNode node;
    node.path = PropertyPath(std::string(property.GetPropertyPath()));
    node.fieldName = std::string(property.GetName());
    node.displayName = std::string(property.GetDisplayName());
    node.kind = GetPropertyKind(property);
    node.metadata = property.GetMetadata();
    node.readOnly = property.IsReadOnly();
    node.valueState = property.GetValueState();
    node.prefabOverride = property.IsPrefabOverride()
        ? PrefabOverrideState::PropertyOverride
        : PrefabOverrideState::None;
    node.targets = targets.targets;
    if (const auto* primary = targets.Primary(); primary != nullptr)
        node.target = *primary;
    if (const auto value = property.GetValue(); value.IsValid())
        node.valueType = value.GetType();

    // Asset references such as Engine::UI::UIDocumentReference persist a value
    // type (an AssetId plus a resource path), so their byte stream node is a
    // structured field. Classify them by the declared value type so the shared
    // object reference drawer owns the picker, drop target, Clear and Ping.
    // Without this the field renders as an expandable struct whose child edits
    // the accessor backend cannot commit, leaving the reference unassignable.
    if (node.valueType.IsValid() && IsSupportedInspectorObjectReferenceType(node.valueType))
        node.kind = InspectorPropertyKind::ObjectReference;

    if (const auto* primary = targets.Primary(); primary != nullptr)
    {
        if (const auto* script = dynamic_cast<const Scripting::ScriptComponent*>(
                NativeReflectionAdapter::ResolveObject(*primary)); script != nullptr)
        {
            ScriptSerializedAdapter::ApplyPropertyPresentation(*script, node);
        }
    }

    if (node.kind == InspectorPropertyKind::Enum && node.metadata.flags)
        node.kind = InspectorPropertyKind::Flags;
    if (node.kind == InspectorPropertyKind::Unsupported && node.diagnostic.empty())
        node.diagnostic = "No safe inspector drawer is registered for '" +
            std::string(property.GetExactTypeName()) + "'.";
    return node;
}
} // namespace

std::vector<PropertyNode> InspectorProjectionBuilder::BuildPropertyProjection(
    Serialization::SerializedObject& serializedObject,
    const InspectorTargetSet& targets,
    const std::string_view searchText)
{
    std::vector<PropertyNode> result;
    auto iterator = serializedObject.GetIterator();
    if (!iterator)
        return result;

    struct Ancestor final
    {
        size_t depth = 0u;
        std::vector<size_t> path;
    };
    const auto resolveNode = [&result](const std::vector<size_t>& path) -> PropertyNode*
    {
        if (path.empty() || path.front() >= result.size())
            return nullptr;
        PropertyNode* node = &result[path.front()];
        for (size_t index = 1u; index < path.size(); ++index)
        {
            if (path[index] >= node->children.size())
                return nullptr;
            node = &node->children[path[index]];
        }
        return node;
    };

    std::vector<Ancestor> ancestors;
    const auto rootDepth = iterator.GetDepth();
    while (iterator.NextVisible(true))
    {
        const auto depth = iterator.GetDepth();
        if (depth < rootDepth)
            continue;

        auto node = BuildNode(iterator, targets);
        if (node.metadata.hideInInspector)
            continue;

        while (!ancestors.empty() && ancestors.back().depth >= depth)
            ancestors.pop_back();

        std::vector<size_t> nodePath;
        if (ancestors.empty())
        {
            result.push_back(std::move(node));
            nodePath = {result.size() - 1u};
        }
        else
        {
            auto* parent = resolveNode(ancestors.back().path);
            if (parent == nullptr)
                continue;
            parent->children.push_back(std::move(node));
            nodePath = ancestors.back().path;
            nodePath.push_back(parent->children.size() - 1u);
        }
        ancestors.push_back({depth, std::move(nodePath)});
    }

    if (!searchText.empty())
    {
        const auto retainMatchingBranch = [&searchText](auto&& self, PropertyNode& node) -> bool
        {
            node.children.erase(
                std::remove_if(node.children.begin(), node.children.end(), [&self](PropertyNode& child)
                {
                    return !self(self, child);
                }),
                node.children.end());
            return MatchesSearch(node, searchText) || !node.children.empty();
        };
        result.erase(
            std::remove_if(result.begin(), result.end(), [&retainMatchingBranch](PropertyNode& node)
            {
                return !retainMatchingBranch(retainMatchingBranch, node);
            }),
            result.end());
    }
    return result;
}
} // namespace NLS::Editor::Inspector
