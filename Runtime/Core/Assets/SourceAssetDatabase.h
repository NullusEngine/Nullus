#pragma once

#include "CoreDef.h"
#include "Assets/AssetDiagnostics.h"
#include "Assets/AssetMeta.h"
#include "Assets/AssetPath.h"

#include <filesystem>
#include <span>
#include <unordered_map>
#include <vector>

namespace NLS::Core::Assets
{
struct NLS_CORE_API SourceAssetRecord
{
    AssetId id;
    std::filesystem::path absolutePath;
    std::filesystem::path relativePath;
    std::filesystem::path metaPath;
    std::string importerId = "unknown";
    uint32_t importerVersion = 1u;
    AssetType assetType = AssetType::Unknown;
    bool readOnly = false;
};

struct NLS_CORE_API SourceAssetRoot
{
    std::filesystem::path path;
    bool readOnly = false;
};

class NLS_CORE_API SourceAssetDatabase
{
public:
    bool ScanRoot(const std::filesystem::path& root, bool readOnly = false);
    bool ScanRoots(std::span<const std::filesystem::path> roots, bool readOnly = false);
    bool ScanRoots(std::span<const SourceAssetRoot> roots);
    bool RegisterSourceAsset(
        const std::filesystem::path& root,
        const std::filesystem::path& assetPath,
        bool readOnly = false,
        std::span<const SourceAssetRoot> mountedRoots = {});
    bool RemoveSourceAsset(const std::filesystem::path& assetPath);

    const std::vector<SourceAssetRecord>& GetRecords() const;
    const AssetDiagnostics& GetDiagnostics() const;

    const SourceAssetRecord* FindById(AssetId id) const;
    const SourceAssetRecord* FindByPath(const std::filesystem::path& path) const;

private:
    void Clear();
    bool ScanRootInternal(const std::filesystem::path& root, bool readOnly);
    bool ScanRootInternal(
        const std::filesystem::path& root,
        bool readOnly,
        std::span<const SourceAssetRoot> mountedRoots);
    void AddDiagnostic(
        AssetDiagnosticSeverity severity,
        std::string code,
        AssetId assetId,
        std::filesystem::path path,
        std::string message);
    bool RegisterSourceAssetEntry(
        const std::filesystem::path& root,
        const std::filesystem::directory_entry& entry,
        bool readOnly,
        std::span<const SourceAssetRoot> mountedRoots = {});
    void RebuildIndexes();

    std::vector<SourceAssetRecord> m_records;
    AssetDiagnostics m_diagnostics;
    std::unordered_map<AssetId, size_t> m_indexById;
    std::unordered_map<std::string, size_t> m_indexByPath;
};
}
