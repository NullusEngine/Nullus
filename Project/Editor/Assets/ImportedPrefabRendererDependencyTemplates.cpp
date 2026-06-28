#include "Assets/ImportedPrefabRendererDependencyTemplates.h"

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "GameObject.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace NLS::Editor::Assets
{
namespace
{
std::unordered_map<NLS::Engine::Serialize::ObjectId, const NLS::Engine::Serialize::ObjectRecord*>
BuildPrefabObjectRecordIndex(const NLS::Engine::Serialize::ObjectGraphDocument& graph)
{
    std::unordered_map<NLS::Engine::Serialize::ObjectId, const NLS::Engine::Serialize::ObjectRecord*> recordsById;
    recordsById.reserve(graph.objects.size());
    for (const auto& object : graph.objects)
        recordsById.emplace(object.id, &object);
    return recordsById;
}

const NLS::Engine::Serialize::PropertyRecord* FindPrefabProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const std::string& name)
{
    for (const auto& property : record.properties)
    {
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

std::vector<NLS::Engine::Serialize::ObjectId> ReadOwnedPrefabArray(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const std::string& propertyName)
{
    std::vector<NLS::Engine::Serialize::ObjectId> ids;
    const auto* property = FindPrefabProperty(record, propertyName);
    if (!property || property->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Array)
        return ids;

    for (const auto& value : property->value.GetArray())
    {
        if (value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
            ids.push_back(value.GetObjectId());
    }
    return ids;
}

template<typename ComponentType>
bool PrefabComponentRecordMatches(const NLS::Engine::Serialize::ObjectRecord& record)
{
    static const std::string typeName = NLS_TYPEOF(ComponentType).GetName();
    return record.typeName == typeName;
}

bool PrefabRecordIsGameObject(const NLS::Engine::Serialize::ObjectRecord& record)
{
    static const std::string typeName = NLS_TYPEOF(NLS::Engine::GameObject).GetName();
    return record.typeName == typeName;
}

struct PrefabResolvedAssetIndex
{
    std::unordered_map<NLS::Core::Assets::AssetId, std::vector<const NLS::Engine::Assets::PrefabResolvedAsset*>> byAssetId;
};

PrefabResolvedAssetIndex BuildPrefabResolvedAssetIndex(
    const NLS::Engine::Assets::PrefabArtifact& prefab)
{
    PrefabResolvedAssetIndex index;
    index.byAssetId.reserve(prefab.resolvedAssets.size());
    for (const auto& resolved : prefab.resolvedAssets)
        index.byAssetId[resolved.assetId].push_back(&resolved);
    return index;
}

std::string NormalizePrefabReferencePath(const std::string& path)
{
    return path.empty()
        ? std::string {}
        : std::filesystem::path(path).lexically_normal().generic_string();
}

bool PrefabResolvedAssetMatchesReferenceHint(
    const NLS::Engine::Assets::PrefabResolvedAsset& resolved,
    const std::string& referencePath,
    const std::string& normalizedReferencePath,
    const std::string_view expectedType)
{
    if (!expectedType.empty() && resolved.expectedType != expectedType)
        return false;

    if (referencePath.empty())
        return true;

    if (resolved.subAssetKey == referencePath)
        return true;

    if (resolved.artifactPath == referencePath)
        return true;

    const auto referenceSubAssetKeyHint =
        NLS::Engine::Assets::ExtractPrefabAssetReferenceSubAssetKeyHint(referencePath);
    if (!referenceSubAssetKeyHint.empty() && resolved.subAssetKey == referenceSubAssetKeyHint)
        return true;

    return !resolved.artifactPath.empty() &&
        NormalizePrefabReferencePath(resolved.artifactPath) == normalizedReferencePath;
}

bool IsBuiltinPrimitiveMeshReferencePath(const std::string& path)
{
    return path.rfind("builtin:Primitive/", 0) == 0;
}

std::optional<std::string> ResolvePrefabAssetPath(
    const PrefabResolvedAssetIndex& resolvedAssets,
    const NLS::Engine::Serialize::PropertyValue& value,
    const std::string_view expectedType)
{
    if (value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::String)
    {
        const auto referencePath = NormalizePrefabReferencePath(value.GetString());
        if (referencePath.empty())
            return std::nullopt;

        const NLS::Engine::Assets::PrefabResolvedAsset* candidate = nullptr;
        for (const auto& [assetId, resolvedList] : resolvedAssets.byAssetId)
        {
            (void)assetId;
            for (const auto* resolved : resolvedList)
            {
                if (!resolved ||
                    !PrefabResolvedAssetMatchesReferenceHint(*resolved, value.GetString(), referencePath, expectedType))
                {
                    continue;
                }

                if (candidate)
                    return std::nullopt;

                candidate = resolved;
            }
        }

        if (candidate)
            return candidate->artifactPath;
        return referencePath;
    }

    if (value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference ||
        !value.GetObjectReference().guid.IsValid())
    {
        return std::nullopt;
    }

    const auto& reference = value.GetObjectReference();
    const auto assetId = NLS::Core::Assets::AssetId(reference.guid);
    const auto referencePath = NormalizePrefabReferencePath(reference.filePath);
    if (expectedType == "Mesh" && IsBuiltinPrimitiveMeshReferencePath(referencePath))
        return referencePath;

    const NLS::Engine::Assets::PrefabResolvedAsset* candidate = nullptr;

    if (const auto foundByAssetId = resolvedAssets.byAssetId.find(assetId);
        foundByAssetId != resolvedAssets.byAssetId.end())
    {
        for (const auto* resolved : foundByAssetId->second)
        {
            if (!resolved ||
                !PrefabResolvedAssetMatchesReferenceHint(*resolved, reference.filePath, referencePath, expectedType))
            {
                continue;
            }

            if (candidate)
                return std::nullopt;

            candidate = resolved;
        }
    }

    if (candidate)
        return candidate->artifactPath;
    return std::nullopt;
}
}

std::vector<ImportedPrefabRendererDependencyTemplate> BuildImportedPrefabRendererDependencyTemplates(
    const NLS::Engine::Assets::PrefabArtifact& prefab)
{
    std::vector<ImportedPrefabRendererDependencyTemplate> templates;
    const auto objectRecordsById = BuildPrefabObjectRecordIndex(prefab.graph);
    const auto resolvedAssetIndex = BuildPrefabResolvedAssetIndex(prefab);

    for (const auto& sourceObject : prefab.graph.objects)
    {
        if (sourceObject.state != NLS::Engine::Serialize::ObjectRecordState::Alive ||
            !PrefabRecordIsGameObject(sourceObject))
        {
            continue;
        }

        ImportedPrefabRendererDependencyTemplate item;
        item.sourceObject = sourceObject.id;
        const auto sourceComponents = ReadOwnedPrefabArray(sourceObject, "components");
        for (const auto& componentId : sourceComponents)
        {
            const auto foundComponentRecord = objectRecordsById.find(componentId);
            if (foundComponentRecord == objectRecordsById.end())
                continue;

            const auto* componentRecord = foundComponentRecord->second;
            if (PrefabComponentRecordMatches<NLS::Engine::Components::MeshFilter>(*componentRecord))
            {
                if (const auto* model = FindPrefabProperty(*componentRecord, "mesh"))
                {
                    if (auto modelPath = ResolvePrefabAssetPath(resolvedAssetIndex, model->value, "Mesh");
                        modelPath.has_value())
                    {
                        item.meshPath = std::move(*modelPath);
                    }
                }
            }
            else if (PrefabComponentRecordMatches<NLS::Engine::Components::MeshRenderer>(*componentRecord))
            {
                if (const auto* materials = FindPrefabProperty(*componentRecord, "materials");
                    materials && materials->value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::Array)
                {
                    item.materialPaths.clear();
                    for (const auto& value : materials->value.GetArray())
                    {
                        auto materialPath = ResolvePrefabAssetPath(resolvedAssetIndex, value, "Material");
                        item.materialPaths.push_back(materialPath.value_or(std::string {}));
                    }
                }
            }
        }

        const bool hasMaterial = std::any_of(
            item.materialPaths.begin(),
            item.materialPaths.end(),
            [](const std::string& path)
            {
                return !path.empty();
            });
        if (!item.meshPath.empty() || hasMaterial)
            templates.push_back(std::move(item));
    }

    return templates;
}
}
