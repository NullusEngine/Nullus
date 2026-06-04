#pragma once

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetDragDropWorkflow.h"
#include "Assets/EditorAssetDragPayload.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace NLS::Editor::Assets
{
struct EditorAssetDragDropBridgeResult
{
    bool handled = false;
    bool pendingImport = false;
    bool importSucceeded = false;
    AssetDragDropResult dragDrop;
};

struct EditorAssetDragDropAsyncRequest
{
    NLS::Core::Assets::AssetId sceneAssetId;
    PrefabInstanceRegistry* prefabInstanceRegistry = nullptr;
    NLS::Engine::GameObject* parent = nullptr;
    ImportProgressTracker* progressTracker = nullptr;
    std::function<void(EditorAssetDragDropBridgeResult)> completion;
    std::function<bool(std::function<void()>)> scheduleBackgroundTask;
};

class EditorAssetDragDropBridge
{
public:
    explicit EditorAssetDragDropBridge(std::filesystem::path projectAssetsPath);

    bool ImportModelIfNeeded(const std::string& resourcePath) const;
    EditorAssetDragDropBridgeResult DropModelAssetIntoHierarchy(
        const std::string& resourcePath,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Core::Assets::AssetId sceneAssetId = {},
        PrefabInstanceRegistry* prefabInstanceRegistry = nullptr,
        NLS::Engine::GameObject* parent = nullptr,
        ImportProgressTracker* progressTracker = nullptr) const;
    EditorAssetDragDropBridgeResult DropModelAssetIntoHierarchyAsync(
        const std::string& resourcePath,
        NLS::Engine::SceneSystem::Scene& scene,
        EditorAssetDragDropAsyncRequest request) const;
    EditorAssetDragDropBridgeResult DropImportedAssetHandleIntoHierarchy(
        const EditorAssetDragPayload& payload,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Core::Assets::AssetId sceneAssetId = {},
        PrefabInstanceRegistry* prefabInstanceRegistry = nullptr,
        NLS::Engine::GameObject* parent = nullptr,
        ImportProgressTracker* progressTracker = nullptr) const;
    EditorAssetDragDropBridgeResult DropImportedPrefabArtifactIntoHierarchy(
        const EditorAssetDragPayload& payload,
        NLS::Engine::Assets::PrefabArtifact& prefab,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Core::Assets::AssetId sceneAssetId = {},
        PrefabInstanceRegistry* prefabInstanceRegistry = nullptr,
        NLS::Engine::GameObject* parent = nullptr,
        ImportProgressTracker* progressTracker = nullptr) const;
    std::optional<NLS::Engine::Assets::PrefabArtifact> TryLoadPreviewPrefabArtifact(
        const EditorAssetDragPayload& payload) const;
    std::optional<NLS::Engine::Assets::PrefabArtifact> TryLoadImportedPrefabArtifact(
        const std::string& assetPath,
        const std::string& prefabSubAssetKey) const;

private:
    std::filesystem::path ProjectRoot() const;
    std::string NormalizeResourcePath(const std::string& resourcePath) const;
    std::string DefaultGeneratedPrefabSubAssetKey(const std::string& assetPath) const;
    std::pair<std::string, std::string> NormalizePrefabResourcePath(const std::string& resourcePath) const;
    EditorAssetDragDropBridgeResult InstantiateImportedAsset(
        NLS::Engine::Assets::PrefabArtifact& prefab,
        const std::string& prefabSubAssetKey,
        NLS::Core::Assets::AssetType assetType,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Core::Assets::AssetId sceneAssetId,
        PrefabInstanceRegistry* prefabInstanceRegistry,
        NLS::Engine::GameObject* parent,
        ImportProgressTracker* progressTracker = nullptr,
        const std::string& progressLabel = {}) const;
    EditorAssetDragDropBridgeResult InstantiateImportedAsset(
        AssetDatabaseFacade& database,
        const std::string& assetPath,
        const std::string& prefabSubAssetKey,
        NLS::Core::Assets::AssetType assetType,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Core::Assets::AssetId sceneAssetId,
        PrefabInstanceRegistry* prefabInstanceRegistry,
        NLS::Engine::GameObject* parent,
        ImportProgressTracker* progressTracker = nullptr,
        const std::string& progressLabel = {}) const;

    std::filesystem::path m_projectAssetsPath;
};
}
