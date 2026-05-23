#include "Engine/Assets/ModelPrefabBuilder.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"

namespace NLS::Engine::Assets
{
namespace
{
using NLS::Engine::Serialize::ObjectGraphDocument;
using NLS::Engine::Serialize::ObjectIdentifier;
using NLS::Engine::Serialize::ObjectId;
using NLS::Engine::Serialize::ObjectRecord;
using NLS::Engine::Serialize::PropertyRecord;
using NLS::Engine::Serialize::PropertyValue;
using NLS::Render::Assets::GeneratedSceneSubAsset;
using NLS::Render::Assets::ImportedScene;
using NLS::Render::Assets::ImportedSceneNamedRecord;
using NLS::Render::Assets::ImportedSceneNode;

const std::string& GameObjectTypeName()
{
    static const std::string name = NLS_TYPEOF(NLS::Engine::GameObject).GetName();
    return name;
}

const std::string& TransformTypeName()
{
    static const std::string name = NLS_TYPEOF(NLS::Engine::Components::TransformComponent).GetName();
    return name;
}

const std::string& MeshRendererTypeName()
{
    static const std::string name = NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName();
    return name;
}

const std::string& MeshFilterTypeName()
{
    static const std::string name = NLS_TYPEOF(NLS::Engine::Components::MeshFilter).GetName();
    return name;
}

std::string MakeLabel(const ImportedScene& scene, const std::string& suffix)
{
    return "GeneratedModelPrefab:" + scene.sourceAssetId.ToString() + ":" + scene.sceneKey + ":" + suffix;
}

ObjectId MakeObjectId(const ImportedScene& scene, const std::string& suffix)
{
    return ObjectId(NLS::Guid::NewDeterministic(MakeLabel(scene, suffix)));
}

PropertyValue MakeVector3(double x, double y, double z)
{
    return PropertyValue::Object({
        {"x", PropertyValue::Number(x)},
        {"y", PropertyValue::Number(y)},
        {"z", PropertyValue::Number(z)}
    });
}

PropertyValue MakeQuaternion(double x, double y, double z, double w)
{
    return PropertyValue::Object({
        {"w", PropertyValue::Number(w)},
        {"x", PropertyValue::Number(x)},
        {"y", PropertyValue::Number(y)},
        {"z", PropertyValue::Number(z)}
    });
}

PropertyValue MakeAssetReference(
    NLS::Core::Assets::AssetId assetId,
    std::string filePath)
{
    const auto localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(
        assetId.GetGuid(),
        filePath);
    return PropertyValue::ObjectReference(ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(assetId.GetGuid()),
        localIdentifierInFile,
        std::move(filePath)));
}

void AddDiagnostic(
    NLS::Engine::Serialize::SerializationDiagnosticList& diagnostics,
    std::string code,
    std::string message)
{
    diagnostics.Add({
        NLS::Engine::Serialize::SerializationDiagnosticCode::UnsupportedFormat,
        NLS::Engine::Serialize::SerializationDiagnosticSeverity::Warning,
        std::move(code) + ": " + std::move(message)
    });
}

const ImportedSceneNamedRecord* FindRecordBySourceKey(
    const std::vector<ImportedSceneNamedRecord>& records,
    const std::string& sourceKey)
{
    const auto found = std::find_if(records.begin(), records.end(), [&sourceKey](const ImportedSceneNamedRecord& record)
    {
        return record.sourceKey == sourceKey;
    });
    return found != records.end() ? &*found : nullptr;
}

std::string SourceKeyOrName(const ImportedSceneNamedRecord& record)
{
    return record.sourceKey.empty() ? record.name : record.sourceKey;
}

std::vector<std::string> CollectMaterialKeysForMesh(
    const ImportedScene& scene,
    const std::string& meshKey)
{
    std::vector<std::string> materialKeys;
    const auto* mesh = FindRecordBySourceKey(scene.meshes, meshKey);
    if (mesh && !mesh->primitives.empty())
    {
        for (const auto& primitive : mesh->primitives)
        {
            if (primitive.materialKey.empty())
                continue;
            const auto material = std::find_if(
                scene.materials.begin(),
                scene.materials.end(),
                [&primitive](const ImportedSceneNamedRecord& candidate)
                {
                    return candidate.sourceKey == primitive.materialKey;
                });
            if (material != scene.materials.end())
            {
                const auto materialSlot = static_cast<size_t>(std::distance(scene.materials.begin(), material));
                if (materialKeys.size() <= materialSlot)
                    materialKeys.resize(materialSlot + 1u);
                materialKeys[materialSlot] = primitive.materialKey;
            }
            else if (std::find(materialKeys.begin(), materialKeys.end(), primitive.materialKey) == materialKeys.end())
            {
                materialKeys.push_back(primitive.materialKey);
            }
        }
        return materialKeys;
    }

    return materialKeys;
}

NLS::Core::Assets::AssetId ResolveSubAssetId(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    NLS::Core::Assets::AssetId fallbackAssetId,
    const std::string& subAssetKey)
{
    const auto* artifact = manifest.FindSubAsset(subAssetKey);
    if (artifact && artifact->sourceAssetId.IsValid())
        return artifact->sourceAssetId;
    return fallbackAssetId;
}

void AddResolvedAsset(
    PrefabArtifact& artifact,
    NLS::Core::Assets::AssetId assetId,
    const std::string& expectedType,
    const std::string& subAssetKey,
    const std::string& artifactPath)
{
    const auto alreadyExists = std::any_of(
        artifact.resolvedAssets.begin(),
        artifact.resolvedAssets.end(),
        [&assetId, &expectedType, &subAssetKey](const PrefabResolvedAsset& existing)
        {
            return existing.assetId == assetId &&
                existing.expectedType == expectedType &&
                existing.subAssetKey == subAssetKey;
        });

    if (!alreadyExists)
        artifact.resolvedAssets.push_back({assetId, expectedType, subAssetKey, artifactPath});
}

std::string ResolveArtifactPath(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::string& subAssetKey)
{
    const auto* artifact = manifest.FindSubAsset(subAssetKey);
    return artifact ? artifact->artifactPath : std::string {};
}

std::string ReadableName(const ImportedSceneNode& node)
{
    if (!node.name.empty())
        return node.name;
    if (!node.sourceKey.empty())
        return node.sourceKey;
    return "Imported Node";
}

std::vector<const ImportedSceneNode*> CollectRootNodes(const ImportedScene& scene)
{
    std::unordered_set<std::string> nodeKeys;
    for (const auto& node : scene.nodes)
        nodeKeys.insert(node.sourceKey);

    std::vector<const ImportedSceneNode*> roots;
    for (const auto& node : scene.nodes)
    {
        if (node.parentKey.empty() || nodeKeys.find(node.parentKey) == nodeKeys.end())
            roots.push_back(&node);
    }
    return roots;
}

PropertyValue MakeOwnedArray(const std::vector<ObjectId>& objectIds)
{
    PropertyValue::ArrayValue values;
    values.reserve(objectIds.size());
    for (const auto& objectId : objectIds)
        values.push_back(PropertyValue::OwnedReference(objectId));
    return PropertyValue::Array(std::move(values));
}

PropertyValue Vector3FromValues(const std::vector<double>& values, const double x, const double y, const double z)
{
    return MakeVector3(
        values.size() > 0u ? values[0] : x,
        values.size() > 1u ? values[1] : y,
        values.size() > 2u ? values[2] : z);
}

PropertyValue QuaternionFromValues(const std::vector<double>& values)
{
    return MakeQuaternion(
        values.size() > 0u ? values[0] : 0.0,
        values.size() > 1u ? values[1] : 0.0,
        values.size() > 2u ? values[2] : 0.0,
        values.size() > 3u ? values[3] : 1.0);
}

ObjectRecord MakeTransformRecord(
    const ImportedSceneNode& node,
    const ObjectId& id,
    const std::string& ownerName)
{
    ObjectRecord transform;
    transform.id = id;
    transform.localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(id);
    transform.typeName = TransformTypeName();
    transform.debugName = ownerName + " Transform";
    transform.properties.push_back({"localPosition", Vector3FromValues(node.translation, 0.0, 0.0, 0.0)});
    transform.properties.push_back({"localRotation", QuaternionFromValues(node.rotation)});
    transform.properties.push_back({"localScale", Vector3FromValues(node.scale, 1.0, 1.0, 1.0)});
    return transform;
}

ObjectRecord MakeMeshFilterRecord(
    const ImportedScene& scene,
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const ObjectId& id,
    const std::string& ownerName,
    const std::string& meshKey,
    PrefabArtifact& artifact)
{
    const auto subAssetKey = std::string("mesh:") + meshKey;
    const auto assetId = ResolveSubAssetId(manifest, scene.sourceAssetId, subAssetKey);
    const auto artifactPath = ResolveArtifactPath(manifest, subAssetKey);
    AddResolvedAsset(artifact, assetId, "Mesh", subAssetKey, artifactPath);

    ObjectRecord meshFilter;
    meshFilter.id = id;
    meshFilter.localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(id);
    meshFilter.typeName = MeshFilterTypeName();
    meshFilter.debugName = ownerName + " MeshFilter";
    meshFilter.properties.push_back({"mesh", MakeAssetReference(
        assetId,
        subAssetKey)});
    return meshFilter;
}

ObjectRecord MakeMeshRendererRecord(
    const ImportedScene& scene,
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const ObjectId& id,
    const std::string& ownerName,
    const std::string& meshKey,
    PrefabArtifact& artifact)
{
    const auto materialKeys = CollectMaterialKeysForMesh(scene, meshKey);
    PropertyValue::ArrayValue materialReferences;
    materialReferences.reserve(materialKeys.size());

    for (const auto& materialKey : materialKeys)
    {
        if (materialKey.empty())
        {
            materialReferences.push_back(PropertyValue::ObjectReference({}));
            continue;
        }

        const auto subAssetKey = std::string("material:") + materialKey;
        const auto assetId = ResolveSubAssetId(manifest, scene.sourceAssetId, subAssetKey);
        const auto artifactPath = ResolveArtifactPath(manifest, subAssetKey);
        AddResolvedAsset(artifact, assetId, "Material", subAssetKey, artifactPath);
        materialReferences.push_back(MakeAssetReference(
            assetId,
            subAssetKey));
    }

    ObjectRecord meshRenderer;
    meshRenderer.id = id;
    meshRenderer.localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(id);
    meshRenderer.typeName = MeshRendererTypeName();
    meshRenderer.debugName = ownerName + " MeshRenderer";
    meshRenderer.properties.push_back({"frustumBehaviour", PropertyValue::String("CULL_MODEL")});
    meshRenderer.properties.push_back({"materials", PropertyValue::Array(std::move(materialReferences))});
    return meshRenderer;
}

void AddRecordMapping(PrefabArtifact& artifact, const ObjectId& sourceObject)
{
    artifact.sourceToRuntimeObject.emplace(
        sourceObject,
        ObjectId(NLS::Guid::NewDeterministic("GeneratedModelPrefab.Runtime:" + sourceObject.GetGuid().ToString())));
}

void AddGeneratedRecords(
    const ImportedScene& scene,
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const ImportedSceneNode& node,
    const ObjectId& gameObjectId,
    const ObjectId& parentId,
    const std::vector<ObjectId>& childIds,
    ObjectGraphDocument& graph,
    PrefabArtifact& artifact)
{
    const auto nodeName = ReadableName(node);
    const auto transformId = MakeObjectId(scene, "component:" + node.sourceKey + ":transform");

    std::vector<ObjectId> components {transformId};

    const auto hasMesh = !node.meshKey.empty();
    const auto meshFilterId = MakeObjectId(scene, "component:" + node.sourceKey + ":mesh-filter");
    const auto meshRendererId = MakeObjectId(scene, "component:" + node.sourceKey + ":mesh-renderer");
    if (hasMesh)
    {
        components.push_back(meshFilterId);
        components.push_back(meshRendererId);
    }

    ObjectRecord gameObject;
    gameObject.id = gameObjectId;
    gameObject.localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(gameObjectId);
    gameObject.typeName = GameObjectTypeName();
    gameObject.debugName = nodeName;
    gameObject.debugPath = "/" + nodeName;
    gameObject.properties.push_back({"active", PropertyValue::Bool(true)});
    gameObject.properties.push_back({"children", MakeOwnedArray(childIds)});
    gameObject.properties.push_back({"components", MakeOwnedArray(components)});
    gameObject.properties.push_back({"name", PropertyValue::String(nodeName)});
    gameObject.properties.push_back({
        "parent",
        parentId.IsValid()
            ? PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(parentId)))
            : PropertyValue::Null()
    });
    gameObject.properties.push_back({"tag", PropertyValue::String({})});

    graph.objects.push_back(std::move(gameObject));
    graph.objects.push_back(MakeTransformRecord(node, transformId, nodeName));
    AddRecordMapping(artifact, gameObjectId);
    AddRecordMapping(artifact, transformId);

    if (hasMesh)
    {
        graph.objects.push_back(MakeMeshFilterRecord(
            scene,
            manifest,
            meshFilterId,
            nodeName,
            node.meshKey,
            artifact));
        graph.objects.push_back(MakeMeshRendererRecord(
            scene,
            manifest,
            meshRendererId,
            nodeName,
            node.meshKey,
            artifact));
        AddRecordMapping(artifact, meshFilterId);
        AddRecordMapping(artifact, meshRendererId);
    }
}
}

PrefabImportResult BuildGeneratedModelPrefab(
    const ImportedScene& scene,
    const std::vector<GeneratedSceneSubAsset>&,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    PrefabImportResult result;
    result.artifact.assetId = scene.sourceAssetId;
    result.artifact.generatedModelPrefab = true;
    result.artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    result.artifact.graph.version = 1;
    result.artifact.graph.documentId = NLS::Guid::NewDeterministic(MakeLabel(scene, "document"));

    if (!scene.skins.empty() || !scene.animations.empty())
    {
        AddDiagnostic(
            result.diagnostics,
            "runtime-skinning-component-missing",
            "Imported skeleton, skin, or animation data is preserved as artifacts but static generated prefabs cannot play it until runtime skinning components exist.");
    }

    if (!scene.morphTargets.empty())
    {
        AddDiagnostic(
            result.diagnostics,
            "runtime-morph-component-missing",
            "Imported morph target data is preserved as artifacts but static generated prefabs cannot play it until runtime morph components exist.");
    }

    std::vector<ImportedSceneNode> nodes = scene.nodes;
    if (nodes.empty())
        nodes.push_back({"scene/root", scene.sceneKey.empty() ? std::string("Imported Model") : scene.sceneKey, "", "", ""});

    std::unordered_map<std::string, ObjectId> nodeObjectIds;
    for (const auto& node : nodes)
        nodeObjectIds.emplace(node.sourceKey, MakeObjectId(scene, "node:" + node.sourceKey));

    std::unordered_map<std::string, std::vector<ObjectId>> childObjectIds;
    for (const auto& node : nodes)
    {
        if (node.parentKey.empty())
            continue;

        const auto foundParent = nodeObjectIds.find(node.parentKey);
        const auto foundChild = nodeObjectIds.find(node.sourceKey);
        if (foundParent != nodeObjectIds.end() && foundChild != nodeObjectIds.end())
            childObjectIds[node.parentKey].push_back(foundChild->second);
    }

    std::vector<const ImportedSceneNode*> roots;
    std::unordered_set<std::string> nodeKeys;
    for (const auto& node : nodes)
        nodeKeys.insert(node.sourceKey);
    for (const auto& node : nodes)
    {
        if (node.parentKey.empty() || nodeKeys.find(node.parentKey) == nodeKeys.end())
            roots.push_back(&node);
    }

    if (roots.size() > 1u)
    {
        const auto syntheticRootId = MakeObjectId(scene, "node:__generated_root");
        result.artifact.graph.root = syntheticRootId;

        std::vector<ObjectId> rootChildren;
        rootChildren.reserve(roots.size());
        for (const auto* root : roots)
            rootChildren.push_back(nodeObjectIds[root->sourceKey]);

        ImportedSceneNode syntheticRoot;
        syntheticRoot.sourceKey = "__generated_root";
        syntheticRoot.name = scene.sceneKey.empty() ? "Imported Model" : scene.sceneKey;
        AddGeneratedRecords(
            scene,
            manifest,
            syntheticRoot,
            syntheticRootId,
            ObjectId(),
            rootChildren,
            result.artifact.graph,
            result.artifact);
    }
    else
    {
        const auto* root = roots.empty() ? &nodes.front() : roots.front();
        result.artifact.graph.root = nodeObjectIds[root->sourceKey];
    }

    std::unordered_set<std::string> syntheticRootChildren;
    for (const auto* root : roots)
        syntheticRootChildren.insert(root->sourceKey);

    for (const auto& node : nodes)
    {
        const auto gameObjectId = nodeObjectIds[node.sourceKey];
        ObjectId parentId;
        const auto foundParent = nodeObjectIds.find(node.parentKey);
        if (foundParent != nodeObjectIds.end())
        {
            parentId = foundParent->second;
        }
        else if (roots.size() > 1u && syntheticRootChildren.find(node.sourceKey) != syntheticRootChildren.end())
        {
            parentId = result.artifact.graph.root;
        }

        AddGeneratedRecords(
            scene,
            manifest,
            node,
            gameObjectId,
            parentId,
            childObjectIds[node.sourceKey],
            result.artifact.graph,
            result.artifact);

        if (!node.skinKey.empty() && FindRecordBySourceKey(scene.skins, node.skinKey) == nullptr)
        {
            AddDiagnostic(
                result.diagnostics,
                "missing-skin-binding",
                "Imported node references a skin key that was not present in the converted scene.");
        }
    }

    const auto graphDiagnostics = result.artifact.Validate();
    for (const auto& diagnostic : graphDiagnostics.GetItems())
        result.diagnostics.Add(diagnostic);

    return result;
}
}
