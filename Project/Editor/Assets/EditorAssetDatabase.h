#pragma once

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetId.h"
#include "Assets/ImportProgressTracker.h"
#include "Core/AssetFileWatcher.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
enum class GeneratedPrefabEditPolicy
{
    ReadOnlyGenerated,
    EditableSource
};

struct GeneratedPrefabState
{
    NLS::Core::Assets::AssetId sourceAssetId;
    std::string subAssetKey;
    GeneratedPrefabEditPolicy editPolicy = GeneratedPrefabEditPolicy::ReadOnlyGenerated;
    bool generatedReadOnly = true;

    bool CanEditInPlace() const;
    bool CanCreateEditableVariant() const;
    bool CanUnpackToSceneObjects() const;
};

struct EditorAssetCommandDescriptor
{
    std::string commandId;
    std::string displayName;
    bool enabled = false;
};

struct EditorImportProgressStatus
{
    bool visible = false;
    std::string label;
    float normalizedProgress = 0.0f;
    bool cancellable = false;
};

struct EditorMaterialViewportBinding
{
    std::string rendererDebugName;
    uint32_t slotIndex = 0u;
    NLS::Engine::Assets::RuntimeAssetRef reference;
    std::string artifactPath;
    bool resolved = false;
};

enum class PrefabCommandSurface
{
    AssetBrowser,
    Inspector
};

enum class PrefabCommandSubject
{
    None,
    SceneSelection,
    SourcePrefabAsset,
    GeneratedModelPrefabAsset,
    PrefabInstance,
    MissingPrefabInstance
};

struct PrefabCommandSurfaceRequest
{
    PrefabCommandSurface surface = PrefabCommandSurface::AssetBrowser;
    PrefabCommandSubject subject = PrefabCommandSubject::None;
    NLS::Core::Assets::AssetId prefabAssetId;
    std::string prefabSubAssetKey;
    bool assetExists = false;
    bool generatedReadOnly = false;
    bool connectedInstance = false;
    bool editableSourceArtifactContext = false;
    size_t overrideCount = 0u;
};

enum class AssetDragDropCommandSubject
{
    Unknown,
    PrefabAssetToHierarchy,
    GeneratedModelPrefabAssetToHierarchy,
    MaterialAssetToRenderer,
    TextureAssetToRenderer,
    HierarchyObjectToAssetFolder,
    PrefabInstanceToAssetFolder,
    GeneratedModelPrefabInstanceToAssetFolder
};

struct AssetDragDropCommandSurfaceRequest
{
    AssetDragDropCommandSubject subject = AssetDragDropCommandSubject::Unknown;
    bool targetExists = false;
    bool targetReadOnly = false;
    bool rendererTarget = false;
    bool editableAssetFolder = false;
    bool generatedReadOnly = false;
};

class EditorAssetDatabase
{
public:
    void RegisterGeneratedPrefab(GeneratedPrefabState state);
    const GeneratedPrefabState* FindGeneratedPrefabState(
        NLS::Core::Assets::AssetId sourceAssetId,
        const std::string& subAssetKey) const;
    std::vector<EditorAssetCommandDescriptor> GetGeneratedPrefabCommands(
        NLS::Core::Assets::AssetId sourceAssetId,
        const std::string& subAssetKey) const;
    std::vector<EditorAssetCommandDescriptor> GetPrefabCommandSurface(
        const PrefabCommandSurfaceRequest& request) const;
    std::vector<EditorAssetCommandDescriptor> GetAssetDragDropCommandSurface(
        const AssetDragDropCommandSurfaceRequest& request) const;
    std::vector<EditorAssetCommandDescriptor> GetImportProgressCommandSurface(
        const ImportBatchProgress& progress) const;
    EditorImportProgressStatus GetImportProgressStatus(
        const std::optional<ImportProgressEvent>& activeEvent) const;
    std::vector<EditorMaterialViewportBinding> GetMaterialPreviewBindings(
        const NLS::Engine::Assets::PrefabArtifact& prefab,
        const NLS::Engine::Assets::RuntimeAssetDatabase& runtimeAssets) const;
    size_t GetGeneratedPrefabStateCount() const;

private:
    std::vector<GeneratedPrefabState> m_generatedPrefabs;
};

enum class AssetRefreshReason
{
    ManualReimport,
    DependencyChanged,
    FileWatcherChanged
};

struct AssetRefreshRequest
{
    std::filesystem::path path;
    AssetRefreshReason reason = AssetRefreshReason::ManualReimport;
};

class AssetRefreshScheduler
{
public:
    void RequestRefresh(std::filesystem::path path, AssetRefreshReason reason);
    void PollWatcher(NLS::Editor::Core::AssetFileWatcher& watcher, const std::filesystem::path& root);
    bool HasPendingRefresh() const;
    std::vector<AssetRefreshRequest> ConsumeRefreshRequests();

private:
    std::vector<AssetRefreshRequest> m_requests;
};

enum class AssetPreimportReason
{
    EditorStartup,
    FileWatcherChanged,
    AssetCopiedOrMoved
};

struct AssetPreimportPlan
{
    std::vector<std::string> assetPaths;
};

struct AssetPreimportRequest
{
    AssetPreimportReason reason = AssetPreimportReason::EditorStartup;
    std::vector<std::filesystem::path> changedPaths;
};

class AssetPreimportScheduler
{
public:
    AssetPreimportPlan BuildPlan(AssetDatabaseFacade& database) const;
    AssetPreimportPlan BuildPlan(AssetDatabaseFacade& database, AssetPreimportReason reason) const;
    AssetPreimportPlan BuildPlan(AssetDatabaseFacade& database, const AssetPreimportRequest& request) const;
    bool Run(AssetDatabaseFacade& database, ImportProgressTracker& progressTracker);
    bool Run(
        AssetDatabaseFacade& database,
        ImportProgressTracker& progressTracker,
        AssetPreimportReason reason);
    bool Run(
        AssetDatabaseFacade& database,
        ImportProgressTracker& progressTracker,
        const AssetPreimportRequest& request);
    bool RunAlreadyPlanned(
        AssetDatabaseFacade& database,
        ImportProgressTracker& progressTracker,
        const AssetPreimportRequest& request,
        const AssetPreimportPlan& plan);
    bool ShouldRunForReason(AssetPreimportReason reason) const;
};
}
