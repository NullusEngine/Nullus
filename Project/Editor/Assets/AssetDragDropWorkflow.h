#pragma once

#include "Assets/AssetId.h"
#include "Assets/AssetVersion.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/PrefabEditorWorkflow.h"
#include "Engine/Assets/PrefabAsset.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
enum class DragPayloadKind
{
    Unknown,
    PrefabAsset,
    GeneratedModelPrefabAsset,
    MaterialAsset,
    TextureAsset,
    HierarchyObject,
    PrefabInstance,
    GeneratedModelPrefabInstance
};

enum class DropTargetKind
{
    Unknown,
    Hierarchy,
    RendererMaterialSlot,
    AssetBrowserFolder
};

enum class DragDropOperationKind
{
    None,
    InstantiatePrefab,
    AssignMaterial,
    CreateMaterialAndAssign,
    SaveAsPrefab,
    CreateVariant,
    CreateUnpackedCopy
};

enum class DragDropOperationStatus
{
    Rejected,
    Failed,
    Committed
};

struct DragPayload
{
    DragPayloadKind kind = DragPayloadKind::Unknown;
    NLS::Core::Assets::AssetId assetId;
    std::string subAssetKey;
    NLS::Engine::Assets::PrefabArtifact* prefab = nullptr;
    NLS::Engine::GameObject* object = nullptr;
    PrefabInstanceRecord* prefabInstance = nullptr;
    std::string assetPath;
    const NLS::Engine::Assets::PrefabArtifact* constPrefab = nullptr;
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> sharedPrefab;
};

struct DropTarget
{
    DropTargetKind kind = DropTargetKind::Unknown;
    NLS::Engine::SceneSystem::Scene* scene = nullptr;
    NLS::Engine::GameObject* parent = nullptr;
    size_t insertionIndex = 0u;
    bool readOnly = false;
    std::filesystem::path assetFolder;
    NLS::Engine::GameObject* materialTarget = nullptr;
};

struct AssetDragDropRequest
{
    DragPayload payload;
    DropTarget target;
    NLS::Core::Assets::AssetId sceneAssetId;
    DragDropOperationKind requestedOperation = DragDropOperationKind::None;
    AssetDatabaseFacade* assetDatabase = nullptr;
    PrefabInstanceRegistry* prefabInstanceRegistry = nullptr;
    NLS::Core::Assets::AssetId destinationAssetId;
    bool deferAssetReferenceResolution = false;
    bool synchronousAssetReferencePrewarm = false;
};

struct AssetDragDropDiagnostic
{
    std::string code;
    std::string message;
};

struct MaterialSlotAssignment
{
    NLS::Engine::GameObject* targetObject = nullptr;
    size_t slot = 0u;
    NLS::Engine::Serialize::ObjectIdentifier material;
};

struct AssetDragDropResult
{
    DragDropOperationStatus status = DragDropOperationStatus::Rejected;
    DragDropOperationKind operation = DragDropOperationKind::None;
    std::vector<AssetDragDropDiagnostic> diagnostics;
    std::optional<PrefabInstanceRecord> instance;
    std::optional<NLS::Engine::Assets::PrefabArtifact> artifact;
    std::vector<NLS::Core::Assets::AssetId> createdAssets;
    std::vector<std::filesystem::path> createdAssetPaths;
    std::vector<NLS::Core::Assets::AssetId> modifiedAssets;
    std::vector<NLS::Core::Assets::AssetId> modifiedScenes;
    std::vector<NLS::Engine::GameObject*> selectedObjects;
    std::vector<MaterialSlotAssignment> materialAssignments;
    std::vector<NLS::Core::Assets::AssetDependencyChange> dependencyChanges;
    std::vector<NLS::Core::Assets::AssetDependencyRecord> dependencyRefreshRequests;
    std::vector<EditorAssetCommandDescriptor> commandDescriptors;
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> sharedArtifact;
    bool deferredAssetReferenceResolutionRequested = false;
};

class AssetDragDropWorkflow
{
public:
    AssetDragDropResult Execute(const AssetDragDropRequest& request) const;

private:
    AssetDragDropResult InstantiatePrefabInHierarchy(const AssetDragDropRequest& request) const;
    AssetDragDropResult AssignMaterialToRenderer(const AssetDragDropRequest& request) const;
    AssetDragDropResult CreateMaterialFromTextureAndAssign(const AssetDragDropRequest& request) const;
    AssetDragDropResult SaveHierarchyObjectAsPrefab(const AssetDragDropRequest& request) const;
    AssetDragDropResult CreatePrefabVariantFromInstance(const AssetDragDropRequest& request) const;
    AssetDragDropResult CreateUnpackedPrefabCopyFromInstance(const AssetDragDropRequest& request) const;
};
}
