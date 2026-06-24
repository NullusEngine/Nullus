#pragma once

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetDragDropWorkflow.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/ImportedPrefabRendererDependencyTemplates.h"
#include "Assets/PrefabUtilityFacade.h"

#include <filesystem>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

enum class UnifiedPrefabLoadMode
{
    SceneRestore,
    DragPreview,
    FinalDrop,
    Duplicate,
    InspectorPreview,
    Prewarm
};

enum class UnifiedPrefabOwnerKind
{
    None,
    SceneInstance,
    PreviewScene,
    Inspector,
    AsyncJob
};

enum class UnifiedPrefabReadiness
{
    PrefabGraphOnly,
    MeshMaterialTextureReady
};

struct UnifiedPrefabLoadRequest
{
    PrefabSourceIdentity source;
    UnifiedPrefabLoadMode loadMode = UnifiedPrefabLoadMode::SceneRestore;
    UnifiedPrefabOwnerKind ownerKind = UnifiedPrefabOwnerKind::None;
    std::string ownerScopeId;
    UnifiedPrefabReadiness requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
    bool allowPending = true;
};

UnifiedPrefabLoadRequest MakeSceneRestoreUnifiedPrefabLoadRequest(
    PrefabSourceIdentity source,
    std::string ownerScopeId);

struct UnifiedPrefabArtifactStamps
{
    std::string manifestStamp;
    std::string dependencyStamp;
    std::string prefabArtifactStamp;
    std::string rendererArtifactStamp;
};

struct PreparedPrefabCacheFreshnessRecord
{
    uint32_t schemaVersion = 0u;
    std::string runtimeCacheIdentity;
    std::string manifestStamp;
    std::string dependencyStamp;
    std::string prefabArtifactStamp;
    std::string rendererArtifactStamp;
    uint32_t prefabImporterVersion = 0u;
    uint32_t reflectionSchemaVersion = 0u;
    uint32_t serializationFormatVersion = 0u;
    uint32_t dependencyManifestVersion = 0u;
};

struct UnifiedPrefabLoadKey
{
    PrefabSourceIdentity source;
    UnifiedPrefabArtifactStamps stamps;
    std::string artifactIdentity;
    std::string runtimeCacheIdentity;
    std::string manifestStamp;
    std::string dependencyStamp;
    std::string prefabArtifactStamp;
    std::string rendererArtifactStamp;
    uint32_t prefabImporterVersion = 0u;
    uint32_t reflectionSchemaVersion = 0u;
    uint32_t serializationFormatVersion = 0u;
    uint32_t dependencyManifestVersion = 0u;

    bool operator==(const UnifiedPrefabLoadKey& other) const
    {
        return runtimeCacheIdentity == other.runtimeCacheIdentity;
    }
};

struct UnifiedPrefabLoadResult
{
    std::optional<NLS::Engine::Assets::PrefabArtifact> prefab;
    std::optional<UnifiedPrefabLoadKey> key;
    bool pending = false;
    bool rendererDependencyMissing = false;
    std::string diagnosticCode;
    std::string diagnosticMessage;
};

struct UnifiedPrefabSharedLoadResult
{
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> prefab;
    std::optional<UnifiedPrefabLoadKey> key;
    bool pending = false;
    bool rendererDependencyMissing = false;
    std::string diagnosticCode;
    std::string diagnosticMessage;
};

std::optional<std::vector<ImportedPrefabRendererDependencyTemplate>>
TryGetImportedPrefabRendererDependencyTemplates(const UnifiedPrefabLoadKey& key);
std::optional<UnifiedPrefabLoadKey> TryGetImportedPrefabLoadKeyForArtifact(
    const NLS::Engine::Assets::PrefabArtifact& prefab);
#if defined(NLS_ENABLE_TEST_HOOKS)
void ClearModelTextureMappingDependencyFingerprintCacheForTesting();
size_t GetModelTextureMappingDependencyFingerprintScanCountForTesting();
std::optional<std::string> ComputeModelTextureMappingDependencyFingerprintForTesting(
    const std::filesystem::path& projectRoot,
    const std::string& dependencyValue,
    const std::string& targetPlatform);
std::vector<std::optional<std::string>> ComputeModelTextureMappingDependencyFingerprintsForTesting(
    const std::filesystem::path& projectRoot,
    const std::vector<std::string>& dependencyValues,
    const std::string& targetPlatform);
#endif
bool SchedulePreviewPrefabHotCachePreload(
    EditorAssetDragPayload payload,
    std::filesystem::path projectAssetsPath,
    std::function<bool(std::function<void()>)> scheduleBackgroundTask);

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
    EditorAssetDragDropBridgeResult DropImportedAssetHandleIntoHierarchyBlocking(
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
    EditorAssetDragDropBridgeResult DropImportedPrefabArtifactIntoHierarchy(
        const EditorAssetDragPayload& payload,
        const NLS::Engine::Assets::PrefabArtifact& prefab,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Core::Assets::AssetId sceneAssetId = {},
        PrefabInstanceRegistry* prefabInstanceRegistry = nullptr,
        NLS::Engine::GameObject* parent = nullptr,
        ImportProgressTracker* progressTracker = nullptr) const;
    std::optional<NLS::Engine::Assets::PrefabArtifact> TryLoadPreviewPrefabArtifact(
        const EditorAssetDragPayload& payload) const;
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> TryLoadPreviewPrefabArtifactShared(
        const EditorAssetDragPayload& payload) const;
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> TryGetCachedPreviewPrefabArtifactShared(
        const EditorAssetDragPayload& payload) const;
    bool PreloadPreviewPrefabHotCache(
        const EditorAssetDragPayload& payload) const;
    bool IsPreviewPrefabArtifactCurrent(
        const EditorAssetDragPayload& payload,
        const NLS::Engine::Assets::PrefabArtifact& prefab,
        UnifiedPrefabReadiness requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly) const;
    std::optional<NLS::Engine::Assets::PrefabArtifact> TryLoadImportedPrefabArtifact(
        const std::string& assetPath,
        const std::string& prefabSubAssetKey) const;
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> TryLoadImportedPrefabArtifactShared(
        const std::string& assetPath,
        const std::string& prefabSubAssetKey) const;
    UnifiedPrefabLoadResult LoadUnifiedPrefab(
        const UnifiedPrefabLoadRequest& request) const;
    UnifiedPrefabSharedLoadResult LoadUnifiedPrefabShared(
        const UnifiedPrefabLoadRequest& request) const;
    std::optional<UnifiedPrefabLoadKey> BuildUnifiedPrefabLoadKey(
        const UnifiedPrefabLoadRequest& request) const;
    bool PreloadPreparedPrefabHotCache(
        const UnifiedPrefabLoadRequest& request) const;

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
        const NLS::Engine::Assets::PrefabArtifact& prefab,
        const std::string& prefabSubAssetKey,
        NLS::Core::Assets::AssetType assetType,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Core::Assets::AssetId sceneAssetId,
        PrefabInstanceRegistry* prefabInstanceRegistry,
        NLS::Engine::GameObject* parent,
        ImportProgressTracker* progressTracker = nullptr,
        const std::string& progressLabel = {},
        std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> sharedPrefab = nullptr) const;
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

#if defined(NLS_ENABLE_TEST_HOOKS)
void ClearImportedPrefabHotCacheForTesting();
size_t GetImportedPrefabHotCacheEntryCountForTesting();
PreparedPrefabCacheFreshnessRecord BuildPreparedPrefabCacheFreshnessRecordForTesting(
    const UnifiedPrefabLoadKey& key);
bool IsPreparedPrefabCacheFreshForTesting(
    const PreparedPrefabCacheFreshnessRecord& record,
    const UnifiedPrefabLoadKey& key);
#endif
}
