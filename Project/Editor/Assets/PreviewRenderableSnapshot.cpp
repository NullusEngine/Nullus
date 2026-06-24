#include "Assets/PreviewRenderableSnapshot.h"

#include "Assets/ImportedPrefabRendererDependencyTemplates.h"
#include "Components/TransformComponent.h"

#include <optional>
#include <unordered_map>

namespace NLS::Editor::Assets
{
namespace
{
struct ParsedPreviewTransform
{
    NLS::Maths::Vector3 localPosition {0.0f, 0.0f, 0.0f};
    NLS::Maths::Quaternion localRotation {NLS::Maths::Quaternion::Identity};
    NLS::Maths::Vector3 localScale {1.0f, 1.0f, 1.0f};
};

const NLS::Engine::Serialize::PropertyRecord* FindProperty(
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

const NLS::Engine::Serialize::PropertyValue* FindObjectValue(
    const NLS::Engine::Serialize::PropertyValue& value,
    const std::string& name)
{
    if (value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Object)
        return nullptr;

    for (const auto& [propertyName, propertyValue] : value.GetObject())
    {
        if (propertyName == name)
            return &propertyValue;
    }
    return nullptr;
}

std::optional<float> ReadFloat(const NLS::Engine::Serialize::PropertyValue& value)
{
    if (value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::Number)
        return static_cast<float>(value.GetNumber());
    if (value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::Integer)
        return static_cast<float>(value.GetInteger());
    return std::nullopt;
}

std::optional<NLS::Maths::Vector3> ReadVector3(
    const NLS::Engine::Serialize::PropertyValue& value)
{
    const auto* x = FindObjectValue(value, "x");
    const auto* y = FindObjectValue(value, "y");
    const auto* z = FindObjectValue(value, "z");
    if (x == nullptr || y == nullptr || z == nullptr)
        return std::nullopt;

    const auto parsedX = ReadFloat(*x);
    const auto parsedY = ReadFloat(*y);
    const auto parsedZ = ReadFloat(*z);
    if (!parsedX.has_value() || !parsedY.has_value() || !parsedZ.has_value())
        return std::nullopt;

    return NLS::Maths::Vector3(*parsedX, *parsedY, *parsedZ);
}

std::optional<NLS::Maths::Quaternion> ReadQuaternion(
    const NLS::Engine::Serialize::PropertyValue& value)
{
    const auto* x = FindObjectValue(value, "x");
    const auto* y = FindObjectValue(value, "y");
    const auto* z = FindObjectValue(value, "z");
    const auto* w = FindObjectValue(value, "w");
    if (x == nullptr || y == nullptr || z == nullptr || w == nullptr)
        return std::nullopt;

    const auto parsedX = ReadFloat(*x);
    const auto parsedY = ReadFloat(*y);
    const auto parsedZ = ReadFloat(*z);
    const auto parsedW = ReadFloat(*w);
    if (!parsedX.has_value() ||
        !parsedY.has_value() ||
        !parsedZ.has_value() ||
        !parsedW.has_value())
    {
        return std::nullopt;
    }

    return NLS::Maths::Quaternion(*parsedX, *parsedY, *parsedZ, *parsedW);
}

ParsedPreviewTransform ReadEmbeddedGameObjectPreviewTransform(
    const NLS::Engine::Serialize::ObjectRecord& record)
{
    ParsedPreviewTransform transform;
    const auto* transformProperty = FindProperty(record, "m_transform");
    if (transformProperty == nullptr)
        return transform;

    if (const auto* localPosition = FindObjectValue(transformProperty->value, "m_localPosition"))
    {
        if (auto parsed = ReadVector3(*localPosition);
            parsed.has_value())
        {
            transform.localPosition = *parsed;
        }
    }

    if (const auto* localRotation = FindObjectValue(transformProperty->value, "m_localRotation"))
    {
        if (auto parsed = ReadQuaternion(*localRotation);
            parsed.has_value())
        {
            transform.localRotation = *parsed;
        }
    }

    if (const auto* localScale = FindObjectValue(transformProperty->value, "m_localScale"))
    {
        if (auto parsed = ReadVector3(*localScale);
            parsed.has_value())
        {
            transform.localScale = *parsed;
        }
    }

    return transform;
}

bool IsTransformComponentRecord(const NLS::Engine::Serialize::ObjectRecord& record)
{
    static const std::string typeName = NLS_TYPEOF(NLS::Engine::Components::TransformComponent).GetName();
    return record.typeName == typeName;
}

std::optional<ParsedPreviewTransform> ReadTransformComponentPreviewTransform(
    const NLS::Engine::Serialize::ObjectRecord& record)
{
    if (!IsTransformComponentRecord(record))
        return std::nullopt;

    ParsedPreviewTransform transform;
    if (const auto* localPosition = FindProperty(record, "localPosition"))
    {
        if (auto parsed = ReadVector3(localPosition->value);
            parsed.has_value())
        {
            transform.localPosition = *parsed;
        }
    }
    if (const auto* localRotation = FindProperty(record, "localRotation"))
    {
        if (auto parsed = ReadQuaternion(localRotation->value);
            parsed.has_value())
        {
            transform.localRotation = *parsed;
        }
    }
    if (const auto* localScale = FindProperty(record, "localScale"))
    {
        if (auto parsed = ReadVector3(localScale->value);
            parsed.has_value())
        {
            transform.localScale = *parsed;
        }
    }
    return transform;
}

std::optional<ParsedPreviewTransform> ReadOwnedTransformComponentPreviewTransform(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, const NLS::Engine::Serialize::ObjectRecord*>& recordsById)
{
    const auto* components = FindProperty(record, "components");
    if (components == nullptr ||
        components->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Array)
    {
        return std::nullopt;
    }

    for (const auto& component : components->value.GetArray())
    {
        if (component.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
            continue;

        const auto found = recordsById.find(component.GetObjectId());
        if (found == recordsById.end() || found->second == nullptr)
            continue;

        if (auto transform = ReadTransformComponentPreviewTransform(*found->second);
            transform.has_value())
        {
            return transform;
        }
    }
    return std::nullopt;
}

ParsedPreviewTransform ReadPreviewTransform(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, const NLS::Engine::Serialize::ObjectRecord*>& recordsById)
{
    if (auto transform = ReadOwnedTransformComponentPreviewTransform(record, recordsById);
        transform.has_value())
    {
        return *transform;
    }

    return ReadEmbeddedGameObjectPreviewTransform(record);
}

std::optional<NLS::Engine::Serialize::ObjectId> ReadPreviewParentObject(
    const NLS::Engine::Serialize::ObjectGraphDocument& graph,
    const NLS::Engine::Serialize::ObjectRecord& record)
{
    const auto* parentProperty = FindProperty(record, "parent");
    if (parentProperty == nullptr ||
        parentProperty->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference)
    {
        return std::nullopt;
    }

    return graph.ResolveObjectReference(parentProperty->value.GetObjectReference());
}

ParsedPreviewTransform CombinePreviewTransforms(
    const ParsedPreviewTransform& parent,
    const ParsedPreviewTransform& child)
{
    const auto parentRotation = NLS::Maths::Quaternion::Normalize(parent.localRotation);
    const NLS::Maths::Vector3 scaledChildPosition {
        child.localPosition.x * parent.localScale.x,
        child.localPosition.y * parent.localScale.y,
        child.localPosition.z * parent.localScale.z
    };

    ParsedPreviewTransform combined;
    combined.localScale = {
        parent.localScale.x * child.localScale.x,
        parent.localScale.y * child.localScale.y,
        parent.localScale.z * child.localScale.z
    };
    combined.localRotation = NLS::Maths::Quaternion::Normalize(parentRotation * child.localRotation);
    combined.localPosition =
        parent.localPosition + NLS::Maths::Quaternion::RotatePoint(scaledChildPosition, parentRotation);
    return combined;
}

ParsedPreviewTransform ReadPreviewWorldTransform(
    const NLS::Engine::Serialize::ObjectGraphDocument& graph,
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, const NLS::Engine::Serialize::ObjectRecord*>& recordsById,
    const NLS::Engine::Serialize::ObjectRecord& record,
    std::unordered_map<NLS::Engine::Serialize::ObjectId, ParsedPreviewTransform>& cache,
    std::unordered_map<NLS::Engine::Serialize::ObjectId, bool>& visiting)
{
    if (const auto cached = cache.find(record.id);
        cached != cache.end())
    {
        return cached->second;
    }

    auto local = ReadPreviewTransform(record, recordsById);
    if (visiting[record.id])
    {
        cache.emplace(record.id, local);
        return local;
    }
    visiting[record.id] = true;

    if (const auto parentId = ReadPreviewParentObject(graph, record);
        parentId.has_value())
    {
        if (const auto parentRecord = recordsById.find(*parentId);
            parentRecord != recordsById.end() && parentRecord->second != nullptr)
        {
            local = CombinePreviewTransforms(
                ReadPreviewWorldTransform(
                    graph,
                    recordsById,
                    *parentRecord->second,
                    cache,
                    visiting),
                local);
        }
    }

    visiting[record.id] = false;
    cache[record.id] = local;
    return local;
}

std::unordered_map<NLS::Engine::Serialize::ObjectId, const NLS::Engine::Serialize::ObjectRecord*>
BuildObjectRecordIndex(const NLS::Engine::Serialize::ObjectGraphDocument& graph)
{
    std::unordered_map<NLS::Engine::Serialize::ObjectId, const NLS::Engine::Serialize::ObjectRecord*> recordsById;
    recordsById.reserve(graph.objects.size());
    for (const auto& object : graph.objects)
        recordsById.emplace(object.id, &object);
    return recordsById;
}

NLS::Core::Assets::AssetId FindResolvedAssetIdForPath(
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    const std::string& artifactPath)
{
    if (artifactPath.empty())
        return {};

    const auto normalizedArtifactPath = std::filesystem::path(artifactPath).lexically_normal().generic_string();
    for (const auto& resolved : prefab.resolvedAssets)
    {
        if (resolved.subAssetKey == artifactPath ||
            resolved.artifactPath == artifactPath ||
            std::filesystem::path(resolved.artifactPath).lexically_normal().generic_string() == normalizedArtifactPath)
        {
            return resolved.assetId;
        }
    }
    return {};
}
}

PreviewRenderableSnapshot BuildPreviewRenderableSnapshot(
    const NLS::Engine::Assets::PrefabArtifact& prefab)
{
    PreviewRenderableSnapshot snapshot;
    const auto templates = BuildImportedPrefabRendererDependencyTemplates(prefab);
    const auto recordsById = BuildObjectRecordIndex(prefab.graph);
    snapshot.drawItems.reserve(templates.size());
    std::unordered_map<NLS::Engine::Serialize::ObjectId, ParsedPreviewTransform> transformCache;
    std::unordered_map<NLS::Engine::Serialize::ObjectId, bool> visitingTransforms;

    for (const auto& item : templates)
    {
        if (item.meshPath.empty())
            continue;

        PreviewDrawItem drawItem;
        drawItem.sourceObject = item.sourceObject;
        drawItem.meshAssetId = FindResolvedAssetIdForPath(prefab, item.meshPath);
        drawItem.meshPath = item.meshPath;
        drawItem.materialAssetIds.reserve(item.materialPaths.size());
        for (const auto& materialPath : item.materialPaths)
            drawItem.materialAssetIds.push_back(FindResolvedAssetIdForPath(prefab, materialPath));
        drawItem.materialPaths = item.materialPaths;
        if (const auto foundRecord = recordsById.find(item.sourceObject);
            foundRecord != recordsById.end() && foundRecord->second != nullptr)
        {
            const auto transform = ReadPreviewWorldTransform(
                prefab.graph,
                recordsById,
                *foundRecord->second,
                transformCache,
                visitingTransforms);
            drawItem.localPosition = transform.localPosition;
            drawItem.localRotation = transform.localRotation;
            drawItem.localScale = transform.localScale;
        }
        snapshot.drawItems.push_back(std::move(drawItem));
    }

    return snapshot;
}
}
