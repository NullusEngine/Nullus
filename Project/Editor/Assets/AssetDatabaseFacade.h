#pragma once

#include "Assets/AssetDiagnostics.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/ImportProgressTracker.h"
#include "Assets/SourceAssetDatabase.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Editor::Assets
{
struct AssetDatabaseRecord
{
    NLS::Core::Assets::AssetId assetId;
    std::string assetPath;
    std::string subAssetKey;
    std::string artifactPath;
    NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    std::string displayName;
    bool mainAsset = false;
};

using EditorArtifactManifestMap =
    std::unordered_map<NLS::Core::Assets::AssetId, NLS::Core::Assets::ArtifactManifest>;
using EditorKnownCurrentAssetPathSet = std::unordered_set<std::string>;

enum class EditorAssetSnapshotStatus
{
    Valid,
    Error,
    Count
};

struct EditorAssetSnapshotDiagnostic
{
    std::string code;
    std::string offendingPath;
    std::string reason;

    bool Empty() const
    {
        return code.empty() && offendingPath.empty() && reason.empty();
    }

    bool operator==(const EditorAssetSnapshotDiagnostic&) const = default;
};

struct EditorSubAssetSnapshot
{
    std::string subAssetKey;
    std::string artifactPath;
    NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    std::string displayName;

    bool operator==(const EditorSubAssetSnapshot&) const = default;
};

struct EditorAssetSnapshot
{
    std::string sourceAssetPath;
    NLS::Core::Assets::AssetId assetId;
    std::vector<EditorSubAssetSnapshot> subAssets;

    bool operator==(const EditorAssetSnapshot&) const = default;
};

using ObjectReferencePickerSubAssetSnapshot = EditorSubAssetSnapshot;
using ObjectReferencePickerAssetSnapshot = EditorAssetSnapshot;

struct EditorAssetSnapshotIndex
{
    EditorAssetSnapshotStatus status = EditorAssetSnapshotStatus::Valid;
    EditorAssetSnapshotDiagnostic diagnostic;
    std::vector<EditorAssetSnapshot> assets;
    std::unordered_map<std::string, size_t> assetIndexByCanonicalSourcePath;
};

struct FacadePublishedState
{
    std::shared_ptr<const EditorArtifactManifestMap> artifactManifests;
    std::shared_ptr<const EditorKnownCurrentAssetPathSet> knownCurrentAssetPaths;
    std::shared_ptr<const EditorAssetSnapshotIndex> snapshotIndex;
};

std::shared_ptr<const EditorAssetSnapshotIndex> BuildValidatedEditorAssetSnapshotIndex(
    std::vector<EditorAssetSnapshot> assets);

struct AssetPackMetadata
{
    std::string name;
    std::string variant;
};

struct AssetObjectRecord
{
    std::string name;
    NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    std::string loaderId;
    std::string serializedPayload;
};

enum class AssetDatabaseAccessMode
{
    Editor,
    Runtime
};

class AssetDatabaseFacade
{
public:
    explicit AssetDatabaseFacade(std::vector<std::filesystem::path> roots);
    explicit AssetDatabaseFacade(std::vector<EditorAssetRoot> roots);
    AssetDatabaseFacade(
        std::vector<std::filesystem::path> roots,
        AssetDatabaseAccessMode mode);
    AssetDatabaseFacade(
        std::vector<EditorAssetRoot> roots,
        AssetDatabaseAccessMode mode);
    static std::shared_ptr<const AssetDatabaseFacade> CreateReadOnlySnapshot(const AssetDatabaseFacade& other);

    AssetDatabaseFacade(const AssetDatabaseFacade& other) = delete;
    AssetDatabaseFacade& operator=(const AssetDatabaseFacade& other) = delete;
    AssetDatabaseFacade(AssetDatabaseFacade&& other) = delete;
    AssetDatabaseFacade& operator=(AssetDatabaseFacade&& other) = delete;

    bool Refresh();
    bool RefreshKnownSourceAssets(std::span<const std::filesystem::path> absoluteAssetPaths);
    bool ImportAsset(const std::string& assetPath);
    bool ImportAsset(const std::string& assetPath, ImportProgressTracker& progressTracker);
    bool ImportAsset(
        const std::string& assetPath,
        ImportProgressTracker& progressTracker,
        size_t batchTotalAssets);
    bool ReimportAsset(const std::string& assetPath);
    bool ReimportAsset(const std::string& assetPath, ImportProgressTracker& progressTracker);
    bool ReimportAsset(
        const std::string& assetPath,
        ImportProgressTracker& progressTracker,
        size_t batchTotalAssets);
    bool ReimportAsset(
        const std::string& assetPath,
        ImportProgressTracker& progressTracker,
        ImportJobId existingJob);
    bool ReimportAssetFromCurrentDatabase(
        const std::string& assetPath,
        ImportProgressTracker& progressTracker,
        size_t batchTotalAssets);
    bool ImportAssetFromCurrentDatabase(
        const std::string& assetPath,
        ImportProgressTracker& progressTracker,
        size_t batchTotalAssets);
    void BeginArtifactDatabaseFlushBatch();
    bool EndArtifactDatabaseFlushBatch();
    void StartAssetEditing();
    bool StopAssetEditing();

    std::string AssetPathToGUID(const std::string& assetPath) const;
    std::string GUIDToAssetPath(const std::string& guid) const;
    std::optional<AssetDatabaseRecord> LoadMainAssetAtPath(const std::string& assetPath) const;
    std::vector<AssetDatabaseRecord> LoadAllAssetsAtPath(const std::string& assetPath) const;
    std::optional<AssetDatabaseRecord> LoadSubAssetAtPath(
        const std::string& assetPath,
        const std::string& subAssetKey) const;
    std::filesystem::path ResolveArtifactPathAtPath(
        const std::string& assetPath,
        const std::string& subAssetKey) const;
    std::optional<NLS::Engine::Assets::PrefabArtifact> LoadPrefabArtifactAtPath(
        const std::string& assetPath,
        const std::string& subAssetKey) const;
    std::optional<NLS::Engine::Assets::PrefabArtifact> LoadPrefabArtifactByAssetId(
        NLS::Core::Assets::AssetId assetId,
        const std::string& subAssetKey) const;

    bool MoveAsset(const std::string& sourceAssetPath, const std::string& destinationAssetPath);
    bool RenameAsset(const std::string& assetPath, const std::string& newName);
    bool CopyAsset(const std::string& sourceAssetPath, const std::string& destinationAssetPath);
    bool DeleteAsset(const std::string& assetPath);
    std::string CreateFolder(const std::string& parentFolder, const std::string& folderName);
    bool IsValidFolder(const std::string& assetPath) const;
    std::string GenerateUniqueAssetPath(const std::string& desiredAssetPath) const;

    void AddArtifactManifest(NLS::Core::Assets::ArtifactManifest manifest);
    std::optional<NLS::Core::Assets::ArtifactManifest> GetArtifactManifestForAssetPath(
        const std::string& assetPath) const;
    bool IsArtifactManifestCurrentForAssetPath(const std::string& assetPath) const;
    bool IsReadOnlyAssetPath(const std::string& assetPath) const;
    bool IsArtifactManifestKnownCurrentForAssetPath(const std::string& assetPath) const;
    std::vector<std::string> GetKnownCurrentArtifactManifestAssetPaths() const;
    std::shared_ptr<const FacadePublishedState> GetPublishedState() const;
    std::vector<ObjectReferencePickerAssetSnapshot> GetObjectReferencePickerAssetSnapshots() const;
    std::vector<ObjectReferencePickerAssetSnapshot> GetFreshObjectReferencePickerAssetSnapshots() const;
    std::shared_ptr<const std::vector<ObjectReferencePickerAssetSnapshot>> GetObjectReferencePickerAssetSnapshotsView() const;
    std::shared_ptr<const std::vector<ObjectReferencePickerAssetSnapshot>> GetFreshObjectReferencePickerAssetSnapshotsView() const;
    void ForEachObjectReferencePickerAssetSnapshot(
        const std::function<void(const ObjectReferencePickerAssetSnapshot&)>& visitor) const;
    void ForEachFreshObjectReferencePickerAssetSnapshot(
        const std::function<void(const ObjectReferencePickerAssetSnapshot&)>& visitor) const;
    const void* GetObjectReferencePickerAssetSnapshotsStorageForTesting() const;
    const void* GetArtifactManifestMapStorageForTesting() const;
    std::optional<std::string> TryGetRootRelativeAssetPath(
        const std::string& ownerAssetPath,
        const std::filesystem::path& path) const;
    std::vector<std::string> GetDependencies(const std::string& assetPath, bool recursive) const;
    std::vector<std::string> FindAssets(
        const std::string& filter,
        const std::vector<std::string>& searchInFolders) const;
    bool SetLabels(const std::string& assetPath, std::vector<std::string> labels);
    std::vector<std::string> GetLabels(const std::string& assetPath) const;
    std::vector<std::string> GetAllLabels() const;
    bool SetAssetPackNameAndVariant(
        const std::string& assetPath,
        std::string packName,
        std::string packVariant);
    std::optional<AssetPackMetadata> GetAssetPackNameAndVariant(const std::string& assetPath) const;
    std::optional<std::string> TryGetEditorAssetPath(const std::filesystem::path& path) const;
    std::vector<NLS::Engine::Assets::AssetPackBuildInput> GetAssetPackBuildInputs() const;
    bool CreateAsset(const AssetObjectRecord& asset, const std::string& assetPath);
    bool CreateTextAsset(
        const std::string& contents,
        const std::string& assetPath,
        NLS::Core::Assets::AssetId assetId = {});
    bool AddObjectToAsset(const AssetObjectRecord& asset, const std::string& assetPath);
    bool ExtractAsset(const AssetDatabaseRecord& asset, const std::string& destinationAssetPath);
    bool Contains(const AssetDatabaseRecord& asset) const;
    bool IsMainAsset(const AssetDatabaseRecord& asset) const;
    bool IsSubAsset(const AssetDatabaseRecord& asset) const;
    bool IsForeignAsset(const AssetDatabaseRecord& asset) const;
    bool IsNativeAsset(const AssetDatabaseRecord& asset) const;
    std::filesystem::path GetArtifactRootForAssetPathForTesting(const std::string& assetPath) const;
    size_t GetQueuedImportCount() const;
    size_t GetCompletedImportCount() const;
    const NLS::Core::Assets::AssetDiagnostics& GetDiagnostics() const;

private:
    using ArtifactManifestMap = EditorArtifactManifestMap;

    std::filesystem::path ResolveAssetPath(const std::string& assetPath) const;
    std::string ToEditorAssetPath(const std::filesystem::path& absolutePath) const;
    const NLS::Core::Assets::SourceAssetRecord* FindRecordByEditorAssetPath(const std::string& assetPath) const;
    NLS::Core::Assets::AssetId ParseAssetId(const std::string& guid) const;
    std::optional<NLS::Core::Assets::AssetMeta> LoadMetaForPath(const std::string& assetPath) const;
    bool SaveMetaForPath(const std::string& assetPath, NLS::Core::Assets::AssetMeta meta);
    bool RefreshSourceDatabase();
    bool RefreshKnownSourceAssetsInternal(std::span<const std::filesystem::path> absoluteAssetPaths);
    bool RefreshSingle(
        const std::string& assetPath,
        ImportProgressTracker* progressTracker = nullptr,
        ImportJobId existingJob = {},
        bool refreshDatabase = true);
    std::filesystem::path ResolveArtifactPathForRecord(
        const NLS::Core::Assets::SourceAssetRecord& record,
        const std::string& artifactPath) const;
    std::filesystem::path GetArtifactRootForAssetPath(const std::filesystem::path& absolutePath) const;
    std::filesystem::path GetArtifactStagingRootForAssetPath(const std::filesystem::path& absolutePath) const;
    std::filesystem::path GetArtifactManifestPathForAssetPath(const std::filesystem::path& absolutePath) const;
    std::filesystem::path GetArtifactDatabasePathForAssetPath(const std::filesystem::path& absolutePath) const;
    void LoadPersistedArtifactManifests();
    std::optional<NLS::Core::Assets::ArtifactManifest> LoadPersistedArtifactManifestForRecord(
        const NLS::Core::Assets::SourceAssetRecord& record) const;
    std::optional<NLS::Core::Assets::ArtifactManifest> LoadPreviousArtifactManifestForRecord(
        const NLS::Core::Assets::SourceAssetRecord& record) const;
    void RefreshKnownCurrentArtifactManifestSnapshot();
    void UpdateKnownCurrentArtifactManifestForAssetPath(const std::string& assetPath);
    bool IsArtifactManifestCurrentForAssetPathUncached(const std::string& assetPath) const;
    std::optional<std::string> ComputeModelTextureMappingDependencyFingerprint(
        const std::string& dependencyValue,
        const std::string& targetPlatform) const;
    static ObjectReferencePickerAssetSnapshot BuildObjectReferencePickerAssetSnapshot(
        const std::string& assetPath,
        const NLS::Core::Assets::ArtifactManifest& manifest);
    void ReplaceKnownCurrentArtifactManifestSnapshotsLocked(
        std::unordered_set<std::string> assetPaths,
        std::vector<ObjectReferencePickerAssetSnapshot> snapshots) const;
    void PublishKnownCurrentArtifactManifestSnapshotForAssetPathLocked(
        const std::string& assetPath,
        const NLS::Core::Assets::ArtifactManifest& manifest);
    void RemoveKnownCurrentArtifactManifestSnapshotForAssetPathLocked(const std::string& assetPath) const;
    void PruneStaleObjectReferencePickerSnapshots() const;
    void PublishCurrentStateLocked() const;
    bool CanSaveArtifactManifestForAssetPath(const std::filesystem::path& absolutePath) const;
    bool SaveArtifactManifestForAssetPath(
        const std::filesystem::path& absolutePath,
        const NLS::Core::Assets::ArtifactManifest& manifest,
        bool deferFlush = false);
    bool EnsureStandardPbrShaderLabSourceAvailable();
    bool SaveArtifactDatabaseManifest(
        const NLS::Core::Assets::ArtifactManifest& manifest,
        bool deferFlush = false);
    bool FlushArtifactDatabaseCache();
    bool FlushArtifactDatabaseCache(std::span<const std::string> databaseKeys);
    void PublishArtifactManifestToMemory(
        NLS::Core::Assets::ArtifactManifest manifest,
        bool markKnownCurrent = false);
    std::string MakeSubAssetKey(const AssetObjectRecord& asset) const;
    NLS::Core::Assets::ImportedArtifact MakeImportedArtifact(
        NLS::Core::Assets::AssetId owner,
        const AssetObjectRecord& asset,
        const std::string& subAssetKey,
        const std::filesystem::path& artifactPath) const;
    bool WriteNativeAssetPayload(
        const std::filesystem::path& absolutePath,
        const AssetObjectRecord& asset,
        const std::string& subAssetKey) const;
    bool IsWritableAssetPath(const std::filesystem::path& absolutePath) const;
    bool IsMutableAssetRecord(const std::string& assetPath) const;
    void RebuildPathIndexes();
    bool RegisterImportedDependencySourceAsset(
        NLS::Core::Assets::AssetId assetId,
        const std::filesystem::path& absolutePath);
    void AddDiagnostic(
        NLS::Core::Assets::AssetDiagnosticSeverity severity,
        std::string code,
        std::filesystem::path path,
        std::string message);
    const ArtifactManifestMap& ManifestsBySource() const;
    ArtifactManifestMap& MutableManifestsBySource() const;
    EditorKnownCurrentAssetPathSet& MutableKnownCurrentArtifactManifestAssetPaths() const;
    bool RejectRuntimeEditorApi(std::string apiName);
    bool IsEditorMode() const;
    bool ImportAsset(
        const std::string& assetPath,
        ImportProgressTracker* progressTracker,
        size_t batchTotalAssets = 1u,
        bool refreshDatabase = true);
    bool ImportAssetImmediateInternal(
        const std::string& assetPath,
        bool refreshDatabase = true);
    bool ReimportAsset(
        const std::string& assetPath,
        ImportProgressTracker* progressTracker,
        ImportJobId existingJob = {},
        size_t batchTotalAssets = 1u,
        bool refreshDatabase = true);

    std::vector<EditorAssetRoot> m_roots;
    AssetDatabaseAccessMode m_mode = AssetDatabaseAccessMode::Editor;
    NLS::Core::Assets::SourceAssetDatabase m_sourceDatabase;
    NLS::Core::Assets::AssetDiagnostics m_diagnostics;
    std::unordered_map<std::string, NLS::Core::Assets::AssetId> m_idByEditorPath;
    std::unordered_map<NLS::Core::Assets::AssetId, std::string> m_editorPathById;
    mutable std::shared_ptr<ArtifactManifestMap> m_manifestsBySource;
    mutable std::shared_ptr<const FacadePublishedState> m_publishedState;
    mutable std::shared_ptr<const uint8_t> m_currentStateIdentity = std::make_shared<const uint8_t>(0u);
    mutable std::shared_ptr<const uint8_t> m_publishedStateIdentity;
    mutable std::recursive_mutex m_manifestMutex;
    mutable std::unordered_map<std::string, NLS::Core::Assets::ArtifactDatabase> m_artifactDatabasesByPath;
    std::unordered_set<std::string> m_dirtyArtifactDatabasePaths;
    mutable std::shared_ptr<EditorKnownCurrentAssetPathSet> m_knownCurrentArtifactManifestAssetPaths;
    mutable std::shared_ptr<const std::vector<ObjectReferencePickerAssetSnapshot>> m_objectReferencePickerAssetSnapshots;
    mutable std::mutex m_artifactDatabaseCacheMutex;
    std::vector<std::string> m_queuedImports;
    bool m_assetEditing = false;
    size_t m_artifactDatabaseFlushBatchDepth = 0u;
    bool m_knownCurrentArtifactManifestSnapshotDirty = false;
    size_t m_completedImports = 0u;
};

#if defined(NLS_ENABLE_TEST_HOOKS)
void ResetAssetDatabaseFullSourceRefreshCountForTesting();
size_t GetAssetDatabaseFullSourceRefreshCountForTesting();
void ResetAssetDatabaseLastKnownSourceRefreshAssetCountForTesting();
size_t GetAssetDatabaseLastKnownSourceRefreshAssetCountForTesting();
void ResetAssetDatabasePersistedManifestLoadCountForTesting();
size_t GetAssetDatabasePersistedManifestLoadCountForTesting();
void ResetAssetDatabaseArtifactManifestCurrentCheckCountForTesting();
size_t GetAssetDatabaseArtifactManifestCurrentCheckCountForTesting();
void ResetAssetDatabaseSourceFileContentHashReadCountForTesting();
size_t GetAssetDatabaseSourceFileContentHashReadCountForTesting();
#endif
}
