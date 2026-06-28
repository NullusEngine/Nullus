#pragma once

#include "Assets/ArtifactManifest.h"
#include "CoreDef.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Core::Assets
{
enum class ArtifactRecordStatus
{
    Unknown,
    Importing,
    UpToDate,
    Stale,
    Failed
};

struct ArtifactDatabaseRecord
{
    AssetId sourceAssetId;
    AssetId artifactSourceAssetId;
    std::string sourcePath;
    std::string subAssetKey;
    ArtifactType artifactType = ArtifactType::Unknown;
    std::string loaderId;
    std::string targetPlatform;
    std::string artifactPath;
    std::string contentHash;
    std::string displayName;
    std::string importerId;
    uint32_t importerVersion = 0u;
    std::string primarySubAssetKey;
    size_t dependencyCount = 0u;
    ArtifactRecordStatus status = ArtifactRecordStatus::Unknown;
};

struct ArtifactDatabaseStats
{
    size_t totalRecords = 0u;
    size_t importingRecords = 0u;
    size_t upToDateRecords = 0u;
    size_t staleRecords = 0u;
    size_t failedRecords = 0u;
};

class NLS_CORE_API ArtifactDatabase
{
public:
    struct ManifestHeader
    {
        AssetId sourceAssetId;
        std::string sourcePath;
        std::string importerId;
        uint32_t importerVersion = 0u;
        std::string targetPlatform;
        std::string primarySubAssetKey;
        size_t dependencyCount = 0u;
        ArtifactRecordStatus status = ArtifactRecordStatus::Unknown;
    };

    ArtifactDatabase() = default;
    ~ArtifactDatabase() = default;

    void Clear();
    void UpsertManifest(
        const ArtifactManifest& manifest,
        std::string sourcePath,
        ArtifactRecordStatus status);
    void MarkStatus(AssetId sourceAssetId, ArtifactRecordStatus status);
    void RemoveSource(AssetId sourceAssetId);
    std::optional<ArtifactManifest> BuildManifestForSource(AssetId sourceAssetId) const;
    std::optional<ArtifactManifest> BuildManifestForSource(
        AssetId sourceAssetId,
        const std::string& targetPlatform) const;

    const ArtifactDatabaseRecord* Find(
        AssetId sourceAssetId,
        const std::string& subAssetKey,
        const std::string& targetPlatform) const;
    std::vector<const ArtifactDatabaseRecord*> FindBySource(AssetId sourceAssetId) const;
    const std::vector<ArtifactDatabaseRecord>& GetRecords() const;
    void VisitRecords(const std::function<void(const ArtifactDatabaseRecord&)>& visitor) const;
    ArtifactDatabaseStats GetStats() const;
#if defined(NLS_ENABLE_TEST_HOOKS)
    size_t GetIndexRebuildCountForTesting() const;
    size_t GetIndexedSourceRecordCountForTesting(AssetId sourceAssetId) const;
    bool MutateRecordForTesting(
        AssetId sourceAssetId,
        const std::string& subAssetKey,
        const std::string& targetPlatform,
        const std::function<void(ArtifactDatabaseRecord&)>& mutator);
    size_t MutateRecordsForTesting(
        const std::function<bool(ArtifactDatabaseRecord&)>& mutator);
#endif

    bool Save(const std::filesystem::path& path) const;
    bool Load(const std::filesystem::path& path);
    std::string GetLastError() const;

private:
    void ClearLastError() const;
    bool Fail(std::string message) const;
    bool FailLmdb(std::string operation, int result) const;
    void RemoveSourceAndUpdateIndex(AssetId sourceAssetId);
    void RemoveSourceTargetAndUpdateIndex(AssetId sourceAssetId, const std::string& targetPlatform);
    void RemoveRecordAt(size_t index);
    void AddRecord(ArtifactDatabaseRecord record);
    void RebuildIndex();
    static std::string MakeKey(
        AssetId sourceAssetId,
        const std::string& subAssetKey,
        const std::string& targetPlatform);

    std::vector<ArtifactDatabaseRecord> m_records;
    std::unordered_map<std::string, ManifestHeader> m_manifestHeadersBySourceTarget;
    std::unordered_map<std::string, std::vector<AssetDependencyRecord>> m_dependenciesBySourceTarget;
    std::unordered_map<std::string, size_t> m_indexByKey;
    std::unordered_map<AssetId, std::vector<size_t>> m_indicesBySourceAssetId;
    size_t m_indexRebuildCountForTesting = 0u;
    mutable std::string m_lastError;
};
}
