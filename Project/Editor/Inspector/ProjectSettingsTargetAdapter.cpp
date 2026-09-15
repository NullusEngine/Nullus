#include "Inspector/ProjectSettingsTargetAdapter.h"

#include <algorithm>
#include <cctype>
#include <functional>

#include <Reflection/Field.h>

#include "Inspector/PropertyTypeTraits.h"
#include "Serialization/ProjectSettingsSerializedBackend.h"
#include "Settings/EditorSettingObject.h"

namespace NLS::Editor::Inspector
{
namespace
{
uint64_t HashCombine(const uint64_t seed, const uint64_t value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool MatchesSearch(const std::string& search, const PropertyNode& node)
{
    if (search.empty())
        return true;
    const auto lower = ToLower(search);
    return ToLower(node.displayName).find(lower) != std::string::npos ||
        ToLower(node.fieldName).find(lower) != std::string::npos ||
        ToLower(node.path.ToString()).find(lower) != std::string::npos ||
        ToLower(node.metadata.tooltip).find(lower) != std::string::npos;
}

void SetNodeTargetSet(PropertyNode& node, const InspectorTargetHandle& target)
{
    node.target = target;
    node.targets = {target};
}
} // namespace

void ProjectSettingsTargetAdapter::BindRegistry(Settings::EditorSettingsRegistry& registry)
{
    Serialization::ProjectSettingsSerializedBackend::BindRegistry(registry);
}

void ProjectSettingsTargetAdapter::UnbindRegistry(const Settings::EditorSettingsRegistry& registry)
{
    Serialization::ProjectSettingsSerializedBackend::UnbindRegistry(registry);
}

bool ProjectSettingsTargetAdapter::CanBuild(const InspectorTargetSet& targets) const
{
    const auto* target = targets.Primary();
    return target != nullptr && target->kind == InspectorTargetKind::ProjectSettings;
}

uint64_t ProjectSettingsTargetAdapter::GetStructureVersion(const InspectorTargetSet& targets) const
{
    const auto* target = targets.Primary();
    if (target == nullptr)
        return 0u;

    uint64_t version = std::hash<std::string> {}(target->resourcePath);
    version = HashCombine(version, std::hash<std::string> {}(target->typeName));
    version = HashCombine(version, Serialization::ProjectSettingsSerializedBackend::GetRegistryRevision());
    if (const auto* setting = Serialization::ProjectSettingsSerializedBackend::ResolveSetting(*target))
    {
        for (const auto& field : setting->type.GetFields())
        {
            if (!field.IsValid())
                continue;
            version = HashCombine(version, std::hash<std::string> {}(field.GetName()));
            version = HashCombine(version, std::hash<std::string> {}(field.GetType().GetName()));
        }
    }
    return version;
}

InspectorPresentation ProjectSettingsTargetAdapter::BuildPresentation(
    const InspectorTargetSet& targets,
    const InspectorBuildContext&) const
{
    InspectorPresentation presentation;
    presentation.targets = targets;
    presentation.targets.Normalize();
    presentation.structureVersion = GetStructureVersion(presentation.targets);
    const auto* target = presentation.targets.Primary();
    const auto* setting = target != nullptr
        ? Serialization::ProjectSettingsSerializedBackend::ResolveSetting(*target)
        : nullptr;
    if (target == nullptr || setting == nullptr)
        return presentation;

    PropertyNode root;
    root.displayName = setting->displayName;
    root.kind = InspectorPropertyKind::Struct;
    root.valueType = setting->type;
    root.structuralVersion = presentation.structureVersion;
    SetNodeTargetSet(root, *target);
    presentation.roots.push_back(std::move(root));
    return presentation;
}
} // namespace NLS::Editor::Inspector
