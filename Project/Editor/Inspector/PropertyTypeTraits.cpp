#include "Inspector/PropertyTypeTraits.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <string_view>

#include <Reflection/RuntimeMetaProperties.h>

#include <Components/Component.h>
#include <GameObject.h>
#include <LayerMask.h>
#include <Math/ExternalReflection.h>
#include <Math/Bounds.h>
#include <Math/BoundsInt.h>
#include <Math/RectInt.h>
#include <Math/Vector2.h>
#include <Math/Vector2Int.h>
#include <Math/Vector3.h>
#include <Math/Vector3Int.h>
#include <Math/Vector4.h>
#include <Rendering/ExternalReflection.h>
#include <Rendering/Geometry/BoundingSphere.h>
#include <Rendering/Geometry/Bounds.h>
#include <Scripting/ScriptTypes.h>

#include "Inspector/ObjectReferenceSupport.h"

namespace NLS::Editor::Inspector
{
namespace
{
void OverlayMetadata(EditorPropertyMetadata& destination, const EditorPropertyMetadata& source)
{
    if (!source.displayName.empty()) destination.displayName = source.displayName;
    if (!source.tooltip.empty()) destination.tooltip = source.tooltip;
    if (!source.header.empty()) destination.header = source.header;
    if (source.spaceBefore != 0.0f) destination.spaceBefore = source.spaceBefore;
    if (source.rangeMin.has_value()) destination.rangeMin = source.rangeMin;
    if (source.rangeMax.has_value()) destination.rangeMax = source.rangeMax;
    if (source.min.has_value()) destination.min = source.min;
    if (source.max.has_value()) destination.max = source.max;
    destination.multiline = destination.multiline || source.multiline;
    destination.readOnly = destination.readOnly || source.readOnly;
    destination.hideInInspector = destination.hideInInspector || source.hideInInspector;
    destination.flags = destination.flags || source.flags;
    destination.colorUsage = destination.colorUsage || source.colorUsage;
    if (!source.referenceTypeConstraint.empty()) destination.referenceTypeConstraint = source.referenceTypeConstraint;
    if (!source.customDrawer.empty()) destination.customDrawer = source.customDrawer;
}

bool HasCanonicalTypeName(
    const meta::Type& type,
    const std::initializer_list<std::string_view> aliases)
{
    const std::string_view name = type.GetName();
    return std::find(aliases.begin(), aliases.end(), name) != aliases.end();
}
} // namespace

InspectorPropertyKind GetInspectorPropertyKind(const meta::Type& type)
{
    if (!type.IsValid())
        return InspectorPropertyKind::Unsupported;
    if (type.IsArray())
        return InspectorPropertyKind::Array;
    if (type.IsEnum())
        return InspectorPropertyKind::Enum;
    if (type == NLS_TYPEOF(bool) || HasCanonicalTypeName(type, {"bool"}))
        return InspectorPropertyKind::Boolean;
    // Generated reflection records C++ spelling, while NLS_TYPEOF() follows
    // the compiler typedef spelling.  Check both so aliases such as uint64_t
    // and unsigned int never fall through to a type-name label in the UI.
    if (type == NLS_TYPEOF(int8_t) || HasCanonicalTypeName(type, {"int8_t", "signed char", "char"}))
        return InspectorPropertyKind::SignedInteger8;
    if (type == NLS_TYPEOF(uint8_t) || HasCanonicalTypeName(type, {"uint8_t", "unsigned char"}))
        return InspectorPropertyKind::UnsignedInteger8;
    if (type == NLS_TYPEOF(int16_t) || HasCanonicalTypeName(type, {"int16_t", "short", "short int", "signed short", "signed short int"}))
        return InspectorPropertyKind::SignedInteger16;
    if (type == NLS_TYPEOF(uint16_t) || HasCanonicalTypeName(type, {"uint16_t", "unsigned short", "unsigned short int"}))
        return InspectorPropertyKind::UnsignedInteger16;
    if (type == NLS_TYPEOF(int32_t) || type == NLS_TYPEOF(int) ||
        HasCanonicalTypeName(type, {"int32_t", "int", "signed", "signed int"}))
        return InspectorPropertyKind::SignedInteger32;
    if (type == NLS_TYPEOF(uint32_t) || type == NLS_TYPEOF(unsigned int) ||
        HasCanonicalTypeName(type, {"uint32_t", "unsigned", "unsigned int"}))
        return InspectorPropertyKind::UnsignedInteger32;
    if (type == NLS_TYPEOF(int64_t) ||
        HasCanonicalTypeName(type, {"int64_t", "long long", "long long int", "__int64", "signed __int64"}))
        return InspectorPropertyKind::SignedInteger64;
    if (type == NLS_TYPEOF(uint64_t) ||
        HasCanonicalTypeName(type, {"uint64_t", "unsigned long long", "unsigned long long int", "unsigned __int64"}))
        return InspectorPropertyKind::UnsignedInteger64;
    if (type == NLS_TYPEOF(float))
        return InspectorPropertyKind::Float;
    if (type == NLS_TYPEOF(double))
        return InspectorPropertyKind::Double;
    if (type == NLS_TYPEOF(std::string))
        return InspectorPropertyKind::String;
    if (type == NLS_TYPEOF(Maths::Vector2))
        return InspectorPropertyKind::Vector2;
    if (type == NLS_TYPEOF(Maths::Vector3))
        return InspectorPropertyKind::Vector3;
    if (type == NLS_TYPEOF(Maths::Vector4))
        return InspectorPropertyKind::Vector4;
    if (type == NLS_TYPEOF(Maths::Quaternion))
        return InspectorPropertyKind::Quaternion;
    if (type == NLS_TYPEOF(Maths::Color))
        return InspectorPropertyKind::Color;
    if (type == NLS_TYPEOF(Maths::Rect))
        return InspectorPropertyKind::Rect;
    if (type == NLS_TYPEOF(Maths::Vector2Int))
        return InspectorPropertyKind::Vector2Int;
    if (type == NLS_TYPEOF(Maths::Vector3Int))
        return InspectorPropertyKind::Vector3Int;
    if (type == NLS_TYPEOF(Maths::RectInt))
        return InspectorPropertyKind::RectInt;
    if (type == NLS_TYPEOF(Maths::Bounds))
        return InspectorPropertyKind::Bounds;
    if (type == NLS_TYPEOF(Maths::BoundsInt))
        return InspectorPropertyKind::BoundsInt;
    if (type == NLS_TYPEOF(Render::Geometry::Bounds))
        return InspectorPropertyKind::Bounds;
    if (type == NLS_TYPEOF(Render::Geometry::BoundingSphere))
        return InspectorPropertyKind::BoundingSphere;
    if (type == NLS_TYPEOF(Engine::LayerMask))
        return InspectorPropertyKind::LayerMask;
    if (IsSupportedInspectorObjectReferenceType(type))
        return InspectorPropertyKind::ObjectReference;
    if (type.IsClass())
        return InspectorPropertyKind::Struct;
    return InspectorPropertyKind::Unsupported;
}

EditorPropertyMetadata BuildEditorPropertyMetadata(const meta::Field& field)
{
    EditorPropertyMetadata metadata;
    metadata.displayName = FormatInspectorDisplayName(field.GetName());
    metadata.readOnly = field.IsReadOnly();

    if (const auto* displayName = field.GetMeta().GetProperty<meta::DisplayName>())
        metadata.displayName = displayName->value;
    if (const auto* tooltip = field.GetMeta().GetProperty<meta::Tooltip>())
        metadata.tooltip = tooltip->value;
    if (const auto* header = field.GetMeta().GetProperty<meta::Header>())
        metadata.header = header->value;
    if (const auto* space = field.GetMeta().GetProperty<meta::Space>())
        metadata.spaceBefore = (std::max)(0.0f, space->value);
    if (const auto* range = field.GetMeta().GetProperty<meta::Range>())
    {
        metadata.rangeMin = (std::min)(range->min, range->max);
        metadata.rangeMax = (std::max)(range->min, range->max);
    }
    if (const auto* minimum = field.GetMeta().GetProperty<meta::Min>())
        metadata.min = minimum->value;
    if (const auto* maximum = field.GetMeta().GetProperty<meta::Max>())
        metadata.max = maximum->value;
    metadata.multiline = field.GetMeta().GetProperty<meta::Multiline>() != nullptr;
    metadata.readOnly = metadata.readOnly || field.GetMeta().GetProperty<meta::ReadOnly>() != nullptr;
    metadata.hideInInspector = field.GetMeta().GetProperty<meta::HideInInspector>() != nullptr;
    metadata.flags = field.GetMeta().GetProperty<meta::Flags>() != nullptr;
    metadata.colorUsage = field.GetMeta().GetProperty<meta::ColorUsage>() != nullptr;
    if (const auto* constraint = field.GetMeta().GetProperty<meta::ReferenceTypeConstraint>())
        metadata.referenceTypeConstraint = constraint->value;

    return InspectorMetadataRegistry::Get().Resolve(field.GetClassType().GetName(), field.GetName(), metadata);
}

EditorPropertyMetadata BuildEditorPropertyMetadata(
    const NLS::Scripting::ScriptFieldDescriptor& field,
    const std::string_view ownerTypeName)
{
    const auto& source = field.metadata;
    EditorPropertyMetadata metadata;
    metadata.displayName = source.displayName;
    metadata.tooltip = source.tooltip;
    metadata.header = source.header;
    metadata.spaceBefore = source.spaceBefore;
    metadata.rangeMin = source.rangeMin;
    metadata.rangeMax = source.rangeMax;
    metadata.min = source.min;
    metadata.max = source.max;
    metadata.multiline = source.multiline;
    metadata.readOnly = source.readOnly;
    metadata.hideInInspector = source.hideInInspector;
    metadata.flags = source.flags;
    metadata.colorUsage = source.colorUsage;
    metadata.referenceTypeConstraint = source.referenceTypeConstraint;
    return BuildEditorPropertyMetadata(ownerTypeName, field.name, std::move(metadata));
}

EditorPropertyMetadata BuildEditorPropertyMetadata(
    const std::string_view ownerTypeName,
    const std::string_view fieldName,
    EditorPropertyMetadata reflectedMetadata)
{
    if (reflectedMetadata.displayName.empty())
        reflectedMetadata.displayName = FormatInspectorDisplayName(fieldName);
    return InspectorMetadataRegistry::Get().Resolve(ownerTypeName, fieldName, reflectedMetadata);
}

std::string FormatInspectorDisplayName(const std::string_view name)
{
    std::string result;
    result.reserve(name.size() + 4u);
    for (size_t index = 0u; index < name.size(); ++index)
    {
        const char current = name[index];
        const bool previousLower = index > 0u && std::islower(static_cast<unsigned char>(name[index - 1u])) != 0;
        const bool nextLower = index + 1u < name.size() && std::islower(static_cast<unsigned char>(name[index + 1u])) != 0;
        if (index > 0u && std::isupper(static_cast<unsigned char>(current)) != 0 &&
            (previousLower || (std::isupper(static_cast<unsigned char>(name[index - 1u])) != 0 && nextLower)))
        {
            result.push_back(' ');
        }

        result.push_back(current == '_' ? ' ' : current);
    }

    if (!result.empty())
        result.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(result.front())));
    return result;
}

InspectorMetadataRegistry& InspectorMetadataRegistry::Get()
{
    static InspectorMetadataRegistry registry;
    return registry;
}

void InspectorMetadataRegistry::Register(std::string typeName, std::string fieldName, EditorPropertyMetadata metadata)
{
    const auto existing = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& entry)
    {
        return entry.typeName == typeName && entry.fieldName == fieldName;
    });

    if (existing != m_entries.end())
        existing->metadata = std::move(metadata);
    else
        m_entries.push_back({std::move(typeName), std::move(fieldName), std::move(metadata)});
    ++m_version;
}

EditorPropertyMetadata InspectorMetadataRegistry::Resolve(
    const meta::Type& type,
    const std::string_view fieldName,
    const EditorPropertyMetadata& reflectedMetadata) const
{
    return Resolve(type.GetName(), fieldName, reflectedMetadata);
}

EditorPropertyMetadata InspectorMetadataRegistry::Resolve(
    const std::string_view typeName,
    const std::string_view fieldName,
    const EditorPropertyMetadata& reflectedMetadata) const
{
    EditorPropertyMetadata result = reflectedMetadata;
    const auto existing = std::find_if(m_entries.rbegin(), m_entries.rend(), [&](const Entry& entry)
    {
        return entry.typeName == typeName && entry.fieldName == fieldName;
    });
    if (existing != m_entries.rend())
        OverlayMetadata(result, existing->metadata);
    return result;
}

uint64_t InspectorMetadataRegistry::GetVersion() const
{
    return m_version;
}
} // namespace NLS::Editor::Inspector
