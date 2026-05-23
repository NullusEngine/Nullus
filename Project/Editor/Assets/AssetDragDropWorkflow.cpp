#include "Assets/AssetDragDropWorkflow.h"

#include "Components/MeshRenderer.h"
#include "Serialize/ObjectGraphWriter.h"

#include <filesystem>
#include <string_view>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
void AddDiagnostic(
    AssetDragDropResult& result,
    std::string code,
    std::string message)
{
    result.diagnostics.push_back({std::move(code), std::move(message)});
}

DragDropOperationStatus ConvertStatus(PrefabEditorOperationStatus status)
{
    switch (status)
    {
    case PrefabEditorOperationStatus::Committed:
        return DragDropOperationStatus::Committed;
    case PrefabEditorOperationStatus::Failed:
        return DragDropOperationStatus::Failed;
    case PrefabEditorOperationStatus::Rejected:
    default:
        return DragDropOperationStatus::Rejected;
    }
}

bool IsPrefabPayload(const DragPayload& payload)
{
    return payload.kind == DragPayloadKind::PrefabAsset ||
        payload.kind == DragPayloadKind::GeneratedModelPrefabAsset;
}

bool IsMaterialPayload(const DragPayload& payload)
{
    return payload.kind == DragPayloadKind::MaterialAsset;
}

bool IsTexturePayload(const DragPayload& payload)
{
    return payload.kind == DragPayloadKind::TextureAsset;
}

bool IsHierarchyObjectPayload(const DragPayload& payload)
{
    return payload.kind == DragPayloadKind::HierarchyObject;
}

bool IsPrefabInstancePayload(const DragPayload& payload)
{
    return payload.kind == DragPayloadKind::PrefabInstance ||
        payload.kind == DragPayloadKind::GeneratedModelPrefabInstance;
}

std::string SanitizeAssetName(std::string name)
{
    for (auto& character : name)
    {
        switch (character)
        {
        case '<':
        case '>':
        case ':':
        case '"':
        case '/':
        case '\\':
        case '|':
        case '?':
        case '*':
            character = '_';
            break;
        default:
            break;
        }
    }
    return name.empty() ? std::string("Prefab") : std::move(name);
}

NLS::Core::Assets::AssetId ResolveDestinationAssetId(const AssetDragDropRequest& request)
{
    if (request.destinationAssetId.IsValid())
        return request.destinationAssetId;
    return NLS::Core::Assets::AssetId::New();
}

std::filesystem::path ResolveDestinationPath(
    const AssetDragDropRequest& request,
    const std::string& defaultName)
{
    const auto folder = request.target.assetFolder.empty()
        ? std::filesystem::path("Assets/Prefabs")
        : request.target.assetFolder;
    const auto desired = (folder / (SanitizeAssetName(defaultName) + ".prefab")).generic_string();
    if (request.assetDatabase)
    {
        const auto unique = request.assetDatabase->GenerateUniqueAssetPath(desired);
        if (!unique.empty())
            return unique;
    }
    return desired;
}

void CopyPrefabOperationOutput(
    AssetDragDropResult& result,
    const PrefabEditorOperationResult& operation)
{
    result.status = ConvertStatus(operation.status);
    for (const auto& diagnostic : operation.diagnostics)
        result.diagnostics.push_back({diagnostic.code, diagnostic.message});
    result.dependencyChanges = operation.dependencyChanges;
    result.dependencyRefreshRequests = operation.dependencyRefreshRequests;
    result.artifact = operation.artifact;
}

void AddSourceAssetRefresh(
    AssetDragDropResult& result,
    const NLS::Core::Assets::AssetId assetId)
{
    if (!assetId.IsValid())
        return;
    result.dependencyRefreshRequests.push_back(
        {NLS::Core::Assets::AssetDependencyKind::SourceAssetGuid, assetId.ToString(), {}});
}

}

AssetDragDropResult AssetDragDropWorkflow::Execute(const AssetDragDropRequest& request) const
{
    if (request.target.readOnly)
    {
        AssetDragDropResult result;
        result.operation = request.requestedOperation;
        AddDiagnostic(
            result,
            "dragdrop-read-only-target",
            "Drag/drop target is read-only and cannot be modified.");
        return result;
    }

    if (request.target.kind == DropTargetKind::Hierarchy && IsPrefabPayload(request.payload))
        return InstantiatePrefabInHierarchy(request);
    if (request.target.kind == DropTargetKind::RendererMaterialSlot && IsMaterialPayload(request.payload))
        return AssignMaterialToRenderer(request);
    if (request.target.kind == DropTargetKind::RendererMaterialSlot && IsTexturePayload(request.payload))
        return CreateMaterialFromTextureAndAssign(request);
    if (request.target.kind == DropTargetKind::AssetBrowserFolder &&
        request.requestedOperation == DragDropOperationKind::SaveAsPrefab &&
        IsHierarchyObjectPayload(request.payload))
    {
        return SaveHierarchyObjectAsPrefab(request);
    }
    if (request.target.kind == DropTargetKind::AssetBrowserFolder &&
        request.requestedOperation == DragDropOperationKind::SaveAsPrefab &&
        request.payload.kind == DragPayloadKind::GeneratedModelPrefabInstance)
    {
        AssetDragDropResult result;
        result.operation = DragDropOperationKind::SaveAsPrefab;
        AddDiagnostic(
            result,
            "dragdrop-generated-artifact-mutation",
            "Generated model prefab instances cannot be saved back over generated artifacts; create a variant or unpacked copy.");
        return result;
    }
    if (request.target.kind == DropTargetKind::AssetBrowserFolder &&
        request.requestedOperation == DragDropOperationKind::CreateVariant &&
        IsPrefabInstancePayload(request.payload))
    {
        return CreatePrefabVariantFromInstance(request);
    }
    if (request.target.kind == DropTargetKind::AssetBrowserFolder &&
        request.requestedOperation == DragDropOperationKind::CreateUnpackedCopy &&
        IsPrefabInstancePayload(request.payload))
    {
        return CreateUnpackedPrefabCopyFromInstance(request);
    }

    AssetDragDropResult result;
    AddDiagnostic(
        result,
        "dragdrop-unsupported-operation",
        "The drag payload cannot be dropped on the selected target.");
    return result;
}

AssetDragDropResult AssetDragDropWorkflow::AssignMaterialToRenderer(
    const AssetDragDropRequest& request) const
{
    AssetDragDropResult result;
    result.operation = DragDropOperationKind::AssignMaterial;

    auto* targetObject = request.target.materialTarget;
    if (!targetObject)
    {
        AddDiagnostic(
            result,
            "dragdrop-missing-material-target",
            "Material drops require a target object.");
        return result;
    }

    if (!targetObject->GetComponent<NLS::Engine::Components::MeshRenderer>())
    {
        AddDiagnostic(
            result,
            "dragdrop-missing-mesh-renderer",
            "Material drops require a target object with a MeshRenderer component.");
        return result;
    }

    if (!request.payload.assetId.IsValid())
    {
        AddDiagnostic(
            result,
            "dragdrop-invalid-material-asset",
            "Material drag payload does not contain a valid material asset id.");
        return result;
    }

    result.materialAssignments.push_back({
        targetObject,
        request.target.insertionIndex,
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            NLS::Engine::Serialize::AssetId(request.payload.assetId.GetGuid()),
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(
                request.payload.assetId.GetGuid(),
                request.payload.subAssetKey.empty() ? std::string_view("material:Main") : std::string_view(request.payload.subAssetKey)),
            request.payload.subAssetKey.empty() ? std::string("material:Main") : request.payload.subAssetKey)
    });
    if (request.sceneAssetId.IsValid())
    {
        result.modifiedScenes.push_back(request.sceneAssetId);
        result.dependencyRefreshRequests.push_back(
            {NLS::Core::Assets::AssetDependencyKind::SourceAssetGuid, request.sceneAssetId.ToString(), {}});
    }
    result.modifiedAssets.push_back(request.payload.assetId);
    result.commandDescriptors.push_back({"dragdrop.assign-material", "Assign Material", true});
    result.status = DragDropOperationStatus::Committed;
    return result;
}

AssetDragDropResult AssetDragDropWorkflow::CreateMaterialFromTextureAndAssign(
    const AssetDragDropRequest& request) const
{
    AssetDragDropResult result;
    result.operation = DragDropOperationKind::CreateMaterialAndAssign;

    if (!request.assetDatabase)
    {
        AddDiagnostic(
            result,
            "dragdrop-missing-asset-database",
            "Texture drops that create materials require an asset database.");
        return result;
    }

    if (!request.payload.assetId.IsValid())
    {
        AddDiagnostic(
            result,
            "dragdrop-invalid-texture-asset",
            "Texture drag payload does not contain a valid texture asset id.");
        return result;
    }

    const auto sourceStem = std::filesystem::path(request.payload.assetPath).stem().generic_string();
    const auto materialName = sourceStem.empty() ? std::string("Material") : sourceStem;
    const auto folder = request.target.assetFolder.empty()
        ? std::filesystem::path("Assets/Materials")
        : request.target.assetFolder;
    const auto desiredPath = (folder / (materialName + ".mat")).generic_string();
    const auto materialPath = request.assetDatabase->GenerateUniqueAssetPath(desiredPath);
    if (materialPath.empty())
    {
        AddDiagnostic(
            result,
            "dragdrop-material-path-unavailable",
            "Could not create a unique material asset path for the texture drop.");
        return result;
    }

    AssetObjectRecord materialAsset;
    materialAsset.name = materialName;
    materialAsset.artifactType = NLS::Core::Assets::ArtifactType::Material;
    materialAsset.loaderId = "material";
    materialAsset.serializedPayload =
        "baseColorTexture=" + request.payload.assetId.ToString() + "#" +
        (request.payload.subAssetKey.empty() ? std::string("texture:Main") : request.payload.subAssetKey);
    if (!request.assetDatabase->CreateAsset(materialAsset, materialPath))
    {
        AddDiagnostic(
            result,
            "dragdrop-material-create-failed",
            "Texture drop could not create the generated material asset.");
        return result;
    }

    const auto materialGuid = request.assetDatabase->AssetPathToGUID(materialPath);
    const auto createdMaterialId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(materialGuid));
    result.createdAssets.push_back(createdMaterialId);
    result.createdAssetPaths.push_back(materialPath);

    auto assignRequest = request;
    assignRequest.payload.kind = DragPayloadKind::MaterialAsset;
    assignRequest.payload.assetId = createdMaterialId;
    assignRequest.payload.subAssetKey = "material:" + materialName;
    auto assigned = AssignMaterialToRenderer(assignRequest);
    assigned.operation = DragDropOperationKind::CreateMaterialAndAssign;
    assigned.createdAssets = std::move(result.createdAssets);
    assigned.createdAssetPaths = std::move(result.createdAssetPaths);
    assigned.modifiedAssets.push_back(createdMaterialId);
    assigned.diagnostics.insert(assigned.diagnostics.begin(), result.diagnostics.begin(), result.diagnostics.end());
    assigned.commandDescriptors.clear();
    assigned.commandDescriptors.push_back({"dragdrop.create-material-and-assign", "Create Material And Assign", true});
    return assigned;
}

AssetDragDropResult AssetDragDropWorkflow::SaveHierarchyObjectAsPrefab(
    const AssetDragDropRequest& request) const
{
    AssetDragDropResult result;
    result.operation = DragDropOperationKind::SaveAsPrefab;

    if (!request.assetDatabase)
    {
        AddDiagnostic(result, "dragdrop-missing-asset-database", "Saving a prefab requires an asset database.");
        return result;
    }

    if (!request.payload.object)
    {
        AddDiagnostic(result, "dragdrop-missing-hierarchy-object", "Saving a prefab requires a hierarchy object.");
        return result;
    }

    const auto destinationId = ResolveDestinationAssetId(request);
    const auto destinationPath = ResolveDestinationPath(request, request.payload.object->GetName());
    auto created = PrefabEditorWorkflow().CreatePrefabFromSelection({
        request.payload.object,
        {},
        destinationId,
        destinationPath
    });
    CopyPrefabOperationOutput(result, created);
    if (result.status != DragDropOperationStatus::Committed || !created.artifact.has_value())
        return result;

    if (!request.assetDatabase->CreateTextAsset(created.prefabSourceText, destinationPath.generic_string(), destinationId))
    {
        result.status = DragDropOperationStatus::Failed;
        AddDiagnostic(result, "dragdrop-prefab-asset-write-failed", "Prefab source asset could not be written.");
        return result;
    }

    result.createdAssets.push_back(destinationId);
    result.createdAssetPaths.push_back(destinationPath);
    result.modifiedAssets.push_back(destinationId);
    AddSourceAssetRefresh(result, destinationId);
    result.commandDescriptors.push_back({"dragdrop.save-as-prefab", "Save As Prefab", true});
    return result;
}

AssetDragDropResult AssetDragDropWorkflow::CreatePrefabVariantFromInstance(
    const AssetDragDropRequest& request) const
{
    AssetDragDropResult result;
    result.operation = DragDropOperationKind::CreateVariant;

    if (!request.assetDatabase)
    {
        AddDiagnostic(result, "dragdrop-missing-asset-database", "Creating a prefab variant requires an asset database.");
        return result;
    }

    if (!request.payload.prefab || !request.payload.prefabInstance)
    {
        AddDiagnostic(result, "dragdrop-missing-prefab-instance", "Creating a variant requires a connected prefab instance.");
        return result;
    }

    const auto destinationId = ResolveDestinationAssetId(request);
    const auto rootName = request.payload.object
        ? request.payload.object->GetName()
        : std::filesystem::path(request.payload.subAssetKey).filename().generic_string();
    const auto destinationPath = ResolveDestinationPath(request, rootName + " Variant");
    auto created = PrefabEditorWorkflow().CreateEditableVariant({
        request.payload.prefab,
        request.payload.prefabInstance->prefabAssetId.IsValid()
            ? request.payload.prefabInstance->prefabAssetId
            : request.payload.assetId,
        request.payload.prefabInstance->prefabSubAssetKey.empty()
            ? request.payload.subAssetKey
            : request.payload.prefabInstance->prefabSubAssetKey,
        destinationPath,
        destinationId,
        request.payload.kind == DragPayloadKind::GeneratedModelPrefabInstance ||
            (request.payload.prefab && request.payload.prefab->generatedModelPrefab),
        false
    });
    CopyPrefabOperationOutput(result, created);
    if (result.status != DragDropOperationStatus::Committed || !created.artifact.has_value())
        return result;

    if (!request.assetDatabase->CreateTextAsset(created.prefabSourceText, destinationPath.generic_string(), destinationId))
    {
        result.status = DragDropOperationStatus::Failed;
        AddDiagnostic(result, "dragdrop-prefab-asset-write-failed", "Prefab variant source asset could not be written.");
        return result;
    }

    result.createdAssets.push_back(destinationId);
    result.createdAssetPaths.push_back(destinationPath);
    result.modifiedAssets.push_back(destinationId);
    AddSourceAssetRefresh(result, destinationId);
    result.commandDescriptors.push_back({"dragdrop.create-variant", "Create Variant", true});
    return result;
}

AssetDragDropResult AssetDragDropWorkflow::CreateUnpackedPrefabCopyFromInstance(
    const AssetDragDropRequest& request) const
{
    AssetDragDropResult result;
    result.operation = DragDropOperationKind::CreateUnpackedCopy;

    if (!request.assetDatabase)
    {
        AddDiagnostic(result, "dragdrop-missing-asset-database", "Creating an unpacked prefab copy requires an asset database.");
        return result;
    }

    if (!request.payload.object)
    {
        AddDiagnostic(result, "dragdrop-missing-hierarchy-object", "Creating an unpacked prefab copy requires a hierarchy object.");
        return result;
    }

    const auto destinationId = ResolveDestinationAssetId(request);
    const auto destinationPath = ResolveDestinationPath(request, request.payload.object->GetName());
    auto created = PrefabEditorWorkflow().CreatePrefabFromSelection({
        request.payload.object,
        {},
        destinationId,
        destinationPath
    });
    CopyPrefabOperationOutput(result, created);
    if (result.status != DragDropOperationStatus::Committed || !created.artifact.has_value())
        return result;

    created.artifact->generatedModelPrefab = false;
    created.artifact->graph.basePrefab.reset();
    created.prefabSourceText = NLS::Engine::Serialize::ObjectGraphWriter::Write(created.artifact->graph);
    result.artifact = created.artifact;

    if (!request.assetDatabase->CreateTextAsset(created.prefabSourceText, destinationPath.generic_string(), destinationId))
    {
        result.status = DragDropOperationStatus::Failed;
        AddDiagnostic(result, "dragdrop-prefab-asset-write-failed", "Unpacked prefab copy source asset could not be written.");
        return result;
    }

    result.createdAssets.push_back(destinationId);
    result.createdAssetPaths.push_back(destinationPath);
    result.modifiedAssets.push_back(destinationId);
    AddSourceAssetRefresh(result, destinationId);
    result.commandDescriptors.push_back({"dragdrop.create-unpacked-copy", "Create Unpacked Copy", true});
    return result;
}

AssetDragDropResult AssetDragDropWorkflow::InstantiatePrefabInHierarchy(
    const AssetDragDropRequest& request) const
{
    AssetDragDropResult result;
    result.operation = DragDropOperationKind::InstantiatePrefab;

    if (!request.target.scene)
    {
        AddDiagnostic(
            result,
            "dragdrop-missing-scene",
            "Dropping a prefab into the Hierarchy requires a target scene.");
        return result;
    }

    if (!request.payload.prefab)
    {
        AddDiagnostic(
            result,
            "dragdrop-missing-prefab",
            "Prefab drag payload does not contain a prefab artifact.");
        return result;
    }

    auto instantiate = PrefabEditorWorkflow().InstantiatePrefab({
        request.payload.prefab,
        request.payload.assetId,
        request.payload.subAssetKey,
        request.sceneAssetId,
        request.deferAssetReferenceResolution
    }, *request.target.scene);

    result.status = ConvertStatus(instantiate.status);
    for (const auto& diagnostic : instantiate.diagnostics)
        result.diagnostics.push_back({diagnostic.code, diagnostic.message});

    if (instantiate.instance.has_value())
    {
        if (request.target.parent && instantiate.instance->instanceRoot)
            instantiate.instance->instanceRoot->SetParent(*request.target.parent);

        if (instantiate.instance->instanceRoot)
            result.selectedObjects.push_back(instantiate.instance->instanceRoot);

        if (request.prefabInstanceRegistry)
            result.instance = request.prefabInstanceRegistry->Register(std::move(*instantiate.instance));
        else
            result.instance = std::move(instantiate.instance);
    }
    result.artifact = *request.payload.prefab;

    result.dependencyRefreshRequests = std::move(instantiate.dependencyRefreshRequests);
    result.dependencyChanges = std::move(instantiate.dependencyChanges);
    if (result.status == DragDropOperationStatus::Committed)
    {
        if (request.sceneAssetId.IsValid())
        {
            result.modifiedScenes.push_back(request.sceneAssetId);
            result.dependencyRefreshRequests.push_back(
                {NLS::Core::Assets::AssetDependencyKind::SourceAssetGuid, request.sceneAssetId.ToString(), {}});
        }
        result.modifiedAssets.push_back(request.payload.assetId);
        result.commandDescriptors.push_back({"dragdrop.instantiate-prefab", "Instantiate Prefab", true});
    }
    return result;
}
}
