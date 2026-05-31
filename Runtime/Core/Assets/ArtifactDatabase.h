#pragma once

#include "Assets/ArtifactManifest.h"
#include "CoreDef.h"

#include <filesystem>
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
    std::string sourcePath;
    std::string subAssetKey;
    ArtifactType artifactType = ArtifactType::Unknown;
    std::string loaderId;
    std::string targetPlatform;
    std::string artifactPath;
    std::string contentHash;
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
    ArtifactDatabase() = default;
    ~ArtifactDatabase() = default;

    void Clear();
    void UpsertManifest(
        const ArtifactManifest& manifest,
        std::string sourcePath,
        ArtifactRecordStatus status);
    void MarkStatus(AssetId sourceAssetId, ArtifactRecordStatus status);
    void RemoveSource(AssetId sourceAssetId);

    const ArtifactDatabaseRecord* Find(
        AssetId sourceAssetId,
        const std::string& subAssetKey,
        const std::string& targetPlatform) const;
    std::vector<const ArtifactDatabaseRecord*> FindBySource(AssetId sourceAssetId) const;
    ArtifactDatabaseStats GetStats() const;
#if defined(NLS_ENABLE_TEST_HOOKS)
    size_t GetIndexRebuildCountForTesting() const;
#endif

    bool Save(const std::filesystem::path& path) const;
    bool Load(const std::filesystem::path& path);

private:
    void RemoveSourceAndUpdateIndex(AssetId sourceAssetId);
    void RemoveRecordAt(size_t index);
    void AddRecord(ArtifactDatabaseRecord record);
    void RebuildIndex();
    static std::string MakeKey(
        AssetId sourceAssetId,
        const std::string& subAssetKey,
        const std::string& targetPlatform);

    std::vector<ArtifactDatabaseRecord> m_records;
    std::unordered_map<std::string, size_t> m_indexByKey;
    size_t m_indexRebuildCountForTesting = 0u;
};
}
