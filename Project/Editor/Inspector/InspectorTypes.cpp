#include "Inspector/InspectorTypes.h"

#include <algorithm>

namespace NLS::Editor::Inspector
{
bool InspectorTargetHandle::IsValid() const
{
    if (IsObjectTarget())
        return objectIdentifier.IsValid() || instanceId != InstanceID_None;

    return kind != InspectorTargetKind::None && (!assetId.empty() || !resourcePath.empty() || !typeName.empty());
}

bool InspectorTargetHandle::IsObjectTarget() const
{
    return kind == InspectorTargetKind::GameObject ||
        kind == InspectorTargetKind::NativeObject ||
        kind == InspectorTargetKind::ScriptObject;
}

bool InspectorTargetHandle::operator==(const InspectorTargetHandle& other) const
{
    const bool sameObjectIdentity =
        objectIdentifier.IsValid() && other.objectIdentifier.IsValid()
            ? objectIdentifier == other.objectIdentifier
            : instanceId == other.instanceId;
    return kind == other.kind &&
        sameObjectIdentity &&
        assetId == other.assetId &&
        resourcePath == other.resourcePath &&
        typeName == other.typeName;
}

InspectorTargetHandle InspectorTargetHandle::FromObject(const Object& object, const InspectorTargetKind kind)
{
    InspectorTargetHandle result;
    result.kind = kind;
    result.instanceId = object.GetInstanceID();
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    if (!persistentManager.InstanceIDToObjectIdentifier(result.instanceId, result.objectIdentifier))
    {
        // Selection may observe a runtime-only object before it has been
        // serialized. Allocate a local persistent identity once so any edit
        // command created from this handle remains safe after a frame boundary.
        result.objectIdentifier = persistentManager.GetOrCreateLocalObjectIdentifier(
            const_cast<Object&>(object));
    }
    result.typeName = object.GetType().GetName();
    return result;
}

InspectorTargetHandle InspectorTargetHandle::FromAsset(
    const InspectorTargetKind kind,
    std::string resourcePath,
    std::string assetId,
    std::string typeName)
{
    InspectorTargetHandle result;
    result.kind = kind;
    result.resourcePath = std::move(resourcePath);
    result.assetId = std::move(assetId);
    result.typeName = std::move(typeName);
    return result;
}

void InspectorTargetSet::Normalize()
{
    targets.erase(
        std::remove_if(targets.begin(), targets.end(), [](const InspectorTargetHandle& target)
        {
            return !target.IsValid();
        }),
        targets.end());

    std::vector<InspectorTargetHandle> normalized;
    normalized.reserve(targets.size());
    for (const auto& target : targets)
    {
        if (std::find(normalized.begin(), normalized.end(), target) == normalized.end())
            normalized.push_back(target);
    }
    targets = std::move(normalized);
}

std::vector<InspectorTargetHandle> GetPropertyNodeTargets(const PropertyNode& node)
{
    auto targets = node.targets;
    if (targets.empty() && node.target.IsValid())
        targets.push_back(node.target);
    return targets;
}

void SetPropertyNodeTargets(
    PropertyNode& node,
    std::vector<InspectorTargetHandle> targets)
{
    if (node.path.Empty())
        return;
    targets.erase(std::remove_if(targets.begin(), targets.end(), [](const auto& target)
        { return !target.IsValid(); }), targets.end());
    std::vector<InspectorTargetHandle> normalized;
    normalized.reserve(targets.size());
    for (const auto& target : targets)
    {
        if (std::find(normalized.begin(), normalized.end(), target) == normalized.end())
            normalized.push_back(target);
    }
    // PropertyNode is a copyable display projection.  The stable target list is
    // presentation identity only; serialization backends resolve their own
    // handles and never consult a global target/binding registry.
    node.targets = std::move(normalized);
}

bool InspectorTargetSet::operator==(const InspectorTargetSet& other) const
{
    return targets == other.targets;
}

PropertyPath PropertyPath::Field(const std::string_view name) const
{
    if (m_value.empty())
        return PropertyPath(std::string(name));
    return PropertyPath(m_value + "." + std::string(name));
}

PropertyPath PropertyPath::Index(const size_t index) const
{
    return PropertyPath(m_value + "[" + std::to_string(index) + "]");
}

PropertyPath PropertyPath::ArraySize() const
{
    return Field("Array").Field("size");
}

PropertyPath PropertyPath::ArrayElement(const size_t index) const
{
    return Field("Array").Field("data").Index(index);
}

bool PropertyPath::IsArraySize() const
{
    constexpr std::string_view suffix = ".Array.size";
    return m_value.size() >= suffix.size() &&
        m_value.compare(m_value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool PropertyPath::IsArrayElement() const
{
    constexpr std::string_view marker = ".Array.data[";
    const auto position = m_value.rfind(marker);
    if (position == std::string::npos || m_value.back() != ']')
        return false;
    return position + marker.size() < m_value.size() - 1u;
}

bool IsIntegerPropertyKind(const InspectorPropertyKind kind)
{
    switch (kind)
    {
    case InspectorPropertyKind::SignedInteger8:
    case InspectorPropertyKind::UnsignedInteger8:
    case InspectorPropertyKind::SignedInteger16:
    case InspectorPropertyKind::UnsignedInteger16:
    case InspectorPropertyKind::SignedInteger32:
    case InspectorPropertyKind::UnsignedInteger32:
    case InspectorPropertyKind::SignedInteger64:
    case InspectorPropertyKind::UnsignedInteger64:
    case InspectorPropertyKind::Enum:
    case InspectorPropertyKind::Flags:
    case InspectorPropertyKind::LayerMask:
        return true;
    default:
        return false;
    }
}

bool IsFloatingPointPropertyKind(const InspectorPropertyKind kind)
{
    return kind == InspectorPropertyKind::Float || kind == InspectorPropertyKind::Double;
}

bool IsNumericPropertyKind(const InspectorPropertyKind kind)
{
    return IsIntegerPropertyKind(kind) || IsFloatingPointPropertyKind(kind);
}
} // namespace NLS::Editor::Inspector
