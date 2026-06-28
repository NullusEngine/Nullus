#include "Assets/SourceAssetDatabase.h"

#include <algorithm>
#include <system_error>

namespace
{
std::string PathKey(const std::filesystem::path& path)
{
    return NLS::Core::Assets::NormalizeAssetPath(path).generic_string();
}

bool IsPathInsideRoot(const std::filesystem::path& candidate, const std::filesystem::path& root)
{
    if (candidate == root)
        return true;

    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative.is_absolute())
        return false;

    for (const auto& part : relative)
    {
        if (part == "..")
            return false;
    }
    return true;
}

bool IsPhysicalPathInsideRoot(const std::filesystem::path& candidate, const std::filesystem::path& root)
{
    std::error_code error;
    const auto canonicalCandidate = std::filesystem::weakly_canonical(candidate, error);
    if (error)
        return false;

    const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error)
        return false;

    return IsPathInsideRoot(canonicalCandidate.lexically_normal(), canonicalRoot.lexically_normal());
}

bool IsReservedAssetCacheDirectoryName(const std::filesystem::path& path)
{
    const auto filename = path.filename().generic_string();
    return filename == "Artifacts" ||
        filename == "ArtifactStaging" ||
        filename == "ArtifactDB" ||
        filename == "ShaderCache" ||
        filename == "BuiltinArtifacts";
}

bool ShouldSkipReservedProjectCacheDirectory(
    const std::filesystem::path& normalizedRoot,
    const std::filesystem::path& directory)
{
    const auto rootName = normalizedRoot.filename().generic_string();
    const auto directoryName = directory.filename().generic_string();
    if (rootName == "Library")
        return true;

    if (rootName == "Assets")
        return false;

    const auto relative = directory.lexically_relative(normalizedRoot);
    if (relative.empty() || relative.is_absolute())
        return false;

    auto begin = relative.begin();
    if (begin == relative.end())
        return false;

    return begin->generic_string() == "Library" &&
        (directoryName == "Library" || IsReservedAssetCacheDirectoryName(directory));
}

bool IsInsideReadOnlyMountedRoot(
    const std::filesystem::path& candidate,
    const std::filesystem::path& currentRoot,
    std::span<const NLS::Core::Assets::SourceAssetRoot> mountedRoots)
{
    for (const auto& mountedRoot : mountedRoots)
    {
        if (!mountedRoot.readOnly)
            continue;

        const auto normalizedMountedRoot = NLS::Core::Assets::NormalizeAssetPath(mountedRoot.path);
        if (normalizedMountedRoot == currentRoot)
            continue;

        if (IsPathInsideRoot(candidate, normalizedMountedRoot) &&
            IsPhysicalPathInsideRoot(candidate, normalizedMountedRoot))
        {
            return true;
        }
    }

    return false;
}
}

namespace NLS::Core::Assets
{
bool SourceAssetDatabase::ScanRoot(const std::filesystem::path& root, const bool readOnly)
{
    Clear();
    return ScanRootInternal(root, readOnly);
}

bool SourceAssetDatabase::ScanRoots(std::span<const std::filesystem::path> roots, const bool readOnly)
{
    Clear();
    std::vector<SourceAssetRoot> mountedRoots;
    mountedRoots.reserve(roots.size());
    for (const auto& root : roots)
        mountedRoots.push_back({ root, readOnly });

    std::vector<SourceAssetRoot> uniqueRoots;
    uniqueRoots.reserve(mountedRoots.size());
    for (const auto& root : mountedRoots)
    {
        const auto rootKey = PathKey(root.path);
        const auto duplicate = std::find_if(
            uniqueRoots.begin(),
            uniqueRoots.end(),
            [&rootKey](const SourceAssetRoot& existingRoot)
            {
                return PathKey(existingRoot.path) == rootKey;
            });
        if (duplicate != uniqueRoots.end())
            continue;

        uniqueRoots.push_back(root);
    }

    bool ok = true;
    for (const auto& root : uniqueRoots)
        ok = ScanRootInternal(root.path, root.readOnly, uniqueRoots) && ok;
    return ok;
}

bool SourceAssetDatabase::ScanRoots(std::span<const SourceAssetRoot> roots)
{
    Clear();
    std::vector<SourceAssetRoot> uniqueRoots;
    uniqueRoots.reserve(roots.size());
    for (const auto& root : roots)
    {
        const auto rootKey = PathKey(root.path);
        const auto duplicate = std::find_if(
            uniqueRoots.begin(),
            uniqueRoots.end(),
            [&rootKey](const SourceAssetRoot& existingRoot)
            {
                return PathKey(existingRoot.path) == rootKey;
            });
        if (duplicate != uniqueRoots.end())
            continue;

        uniqueRoots.push_back(root);
    }

    bool ok = true;
    for (const auto& root : uniqueRoots)
        ok = ScanRootInternal(root.path, root.readOnly, uniqueRoots) && ok;
    return ok;
}

bool SourceAssetDatabase::ScanRootInternal(const std::filesystem::path& root, const bool readOnly)
{
    return ScanRootInternal(root, readOnly, {});
}

bool SourceAssetDatabase::ScanRootInternal(
    const std::filesystem::path& root,
    const bool readOnly,
    std::span<const SourceAssetRoot> mountedRoots)
{

    std::error_code error;
    const auto normalizedRoot = NormalizeAssetPath(root);
    if (root.empty() ||
        normalizedRoot.empty() ||
        normalizedRoot == normalizedRoot.root_path() ||
        !std::filesystem::exists(normalizedRoot, error) ||
        !std::filesystem::is_directory(normalizedRoot, error))
    {
        AddDiagnostic(
            AssetDiagnosticSeverity::Error,
            "asset-root-invalid",
            AssetId(),
            root,
            "Asset root does not exist or is not a directory.");
        return false;
    }

    std::vector<std::filesystem::path> orphanMetaCandidates;
    {
        std::filesystem::recursive_directory_iterator iterator(normalizedRoot, error);
        if (error)
        {
            AddDiagnostic(
                AssetDiagnosticSeverity::Error,
                "asset-root-unreadable",
                AssetId(),
                normalizedRoot,
                "Asset root could not be enumerated.");
            return false;
        }

        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end; iterator.increment(error))
        {
            if (error)
            {
                AddDiagnostic(
                    AssetDiagnosticSeverity::Warning,
                    "asset-scan-entry-skipped",
                    AssetId(),
                    normalizedRoot,
                    "An asset entry could not be read during scan.");
                error.clear();
                continue;
            }

            const auto& entry = *iterator;
            if (entry.is_directory(error))
            {
                const auto path = entry.path().lexically_normal();
                if (ShouldSkipReservedProjectCacheDirectory(normalizedRoot, path))
                    iterator.disable_recursion_pending();
                error.clear();
                continue;
            }
            if (error)
            {
                error.clear();
                continue;
            }

            if (!entry.is_regular_file(error) || error)
            {
                error.clear();
                continue;
            }

            const auto path = entry.path().lexically_normal();
            if (IsMetaFilePath(path))
            {
                orphanMetaCandidates.push_back(path);
                continue;
            }

            if (!RegisterSourceAssetEntry(normalizedRoot, entry, readOnly, mountedRoots))
                return false;
        }
    }

    if (!readOnly)
    {
        for (const auto& metaPath : orphanMetaCandidates)
        {
            if (IsInsideReadOnlyMountedRoot(metaPath, normalizedRoot, mountedRoots))
                continue;

            const auto sourcePath = metaPath.parent_path() / metaPath.stem();

            std::error_code sourceError;
            if (std::filesystem::exists(sourcePath, sourceError))
                continue;
            if (sourceError)
            {
                AddDiagnostic(
                    AssetDiagnosticSeverity::Warning,
                    "asset-meta-orphan-check-failed",
                    AssetId(),
                    metaPath,
                    "Asset metadata orphan check could not be completed.");
                continue;
            }

            std::error_code removeError;
            std::filesystem::remove(metaPath, removeError);
            if (removeError)
            {
                AddDiagnostic(
                    AssetDiagnosticSeverity::Warning,
                    "asset-meta-orphan-delete-failed",
                    AssetId(),
                    metaPath,
                    "Stale asset metadata could not be deleted.");
            }
        }
    }

    return true;
}

const std::vector<SourceAssetRecord>& SourceAssetDatabase::GetRecords() const
{
    return m_records;
}

const AssetDiagnostics& SourceAssetDatabase::GetDiagnostics() const
{
    return m_diagnostics;
}

const SourceAssetRecord* SourceAssetDatabase::FindById(const AssetId id) const
{
    const auto found = m_indexById.find(id);
    if (found == m_indexById.end())
        return nullptr;
    return &m_records[found->second];
}

const SourceAssetRecord* SourceAssetDatabase::FindByPath(const std::filesystem::path& path) const
{
    const auto found = m_indexByPath.find(PathKey(path));
    if (found == m_indexByPath.end())
        return nullptr;
    return &m_records[found->second];
}

void SourceAssetDatabase::Clear()
{
    m_records.clear();
    m_diagnostics.clear();
    m_indexById.clear();
    m_indexByPath.clear();
}

void SourceAssetDatabase::AddDiagnostic(
    const AssetDiagnosticSeverity severity,
    std::string code,
    const AssetId assetId,
    std::filesystem::path path,
    std::string message)
{
    m_diagnostics.push_back({
        severity,
        std::move(code),
        assetId,
        std::move(path),
        std::move(message)
    });
}

bool SourceAssetDatabase::RegisterSourceAsset(
    const std::filesystem::path& root,
    const std::filesystem::path& assetPath,
    const bool readOnly,
    std::span<const SourceAssetRoot> mountedRoots)
{
    std::error_code error;
    const auto normalizedPath = NormalizeAssetPath(assetPath);
    if (!std::filesystem::is_regular_file(normalizedPath, error) || error)
    {
        AddDiagnostic(
            AssetDiagnosticSeverity::Warning,
            "asset-scan-entry-skipped",
            AssetId(),
            normalizedPath,
            "Asset entry could not be read during scan.");
        return false;
    }

    return RegisterSourceAssetEntry(root, std::filesystem::directory_entry(normalizedPath, error), readOnly, mountedRoots);
}

bool SourceAssetDatabase::RegisterSourceAssetEntry(
    const std::filesystem::path& root,
    const std::filesystem::directory_entry& entry,
    const bool readOnly,
    std::span<const SourceAssetRoot> mountedRoots)
{
    const auto absolutePath = NormalizeAssetPath(entry.path());
    if (IsMetaFilePath(absolutePath))
        return true;

    const auto normalizedRoot = NormalizeAssetPath(root);
    if (!IsPathInsideRoot(absolutePath, normalizedRoot) ||
        !IsPhysicalPathInsideRoot(absolutePath, normalizedRoot))
    {
        AddDiagnostic(
            AssetDiagnosticSeverity::Warning,
            "asset-scan-entry-outside-root",
            AssetId(),
            absolutePath,
            "Asset entry resolves outside its mounted source root and was skipped.");
        return true;
    }

    for (const auto& mountedRoot : mountedRoots)
    {
        const auto normalizedMountedRoot = NormalizeAssetPath(mountedRoot.path);
        if (normalizedMountedRoot == normalizedRoot)
            continue;

        const auto mountedRootRelativeToRoot = normalizedMountedRoot.lexically_relative(normalizedRoot);
        if (mountedRootRelativeToRoot.empty() || mountedRootRelativeToRoot.is_absolute())
            continue;

        bool mountedRootIsChild = true;
        for (const auto& part : mountedRootRelativeToRoot)
        {
            if (part == "..")
            {
                mountedRootIsChild = false;
                break;
            }
        }
        if (!mountedRootIsChild)
            continue;

        const auto relativeToMountedRoot = absolutePath.lexically_relative(normalizedMountedRoot);
        if (relativeToMountedRoot.empty() || relativeToMountedRoot.is_absolute())
            continue;

        bool insideMountedRoot = true;
        for (const auto& part : relativeToMountedRoot)
        {
            if (part == "..")
            {
                insideMountedRoot = false;
                break;
            }
        }

        if (insideMountedRoot)
            return true;
    }

    auto metaPath = GetAssetMetaPath(absolutePath);
    const auto loadedMeta = AssetMeta::Load(metaPath);
    auto meta = loadedMeta.value_or(AssetMeta::CreateForAsset(absolutePath));
    const auto relativePath = std::filesystem::relative(absolutePath, normalizedRoot);
    if (readOnly && !loadedMeta.has_value())
    {
        meta.id = AssetId(NLS::Guid::NewDeterministic(
            "readonly-asset:" + relativePath.lexically_normal().generic_string()));
    }

    const auto inferredType = InferAssetType(absolutePath);
    if (inferredType != AssetType::Unknown && meta.assetType != inferredType)
    {
        meta.assetType = inferredType;
        meta.importerId = InferImporterId(meta.assetType);
    }
    else if (meta.assetType == AssetType::Unknown)
    {
        meta.assetType = inferredType;
    }
    if (meta.importerId.empty() || meta.importerId == "unknown")
        meta.importerId = InferImporterId(meta.assetType);
    meta.importerVersion = std::max(meta.importerVersion, GetCurrentImporterVersion(meta.assetType));
    if (!meta.id.IsValid())
        meta.id = AssetId::New();

    if (!readOnly && (!loadedMeta.has_value() || *loadedMeta != meta))
    {
        if (!meta.Save(metaPath))
        {
            AddDiagnostic(
                AssetDiagnosticSeverity::Error,
                "asset-meta-write-failed",
                meta.id,
                metaPath,
                "Failed to write asset metadata sidecar.");
            return false;
        }
    }
    else if (!readOnly && !std::filesystem::exists(metaPath))
    {
        AddDiagnostic(
            AssetDiagnosticSeverity::Warning,
            "readonly-asset-missing-meta",
            meta.id,
            absolutePath,
            "Read-only asset is missing metadata; generated identity is scan-local.");
    }

    if (const auto duplicate = m_indexById.find(meta.id); duplicate != m_indexById.end())
    {
        if (readOnly)
        {
            AddDiagnostic(
                AssetDiagnosticSeverity::Error,
                "duplicate-asset-guid",
                meta.id,
                absolutePath,
                "Duplicate asset GUID found while scanning read-only source assets.");
            return true;
        }

        const auto duplicatedId = meta.id;
        meta.id = AssetId::New();
        if (!meta.Save(metaPath))
        {
            AddDiagnostic(
                AssetDiagnosticSeverity::Error,
                "asset-meta-write-failed",
                duplicatedId,
                metaPath,
                "Failed to repair duplicate asset metadata sidecar.");
            return false;
        }

        AddDiagnostic(
            AssetDiagnosticSeverity::Warning,
            "duplicate-asset-guid-repaired",
            meta.id,
            absolutePath,
            "Duplicate asset GUID found while scanning source assets; generated a new editable metadata identity.");
    }

    SourceAssetRecord record;
    record.id = meta.id;
    record.absolutePath = absolutePath;
    record.relativePath = relativePath.lexically_normal();
    record.metaPath = metaPath;
    record.importerId = meta.importerId;
    record.importerVersion = meta.importerVersion;
    record.assetType = meta.assetType;
    record.readOnly = readOnly;

    const auto index = m_records.size();
    m_indexById.emplace(record.id, index);
    m_indexByPath.emplace(PathKey(record.absolutePath), index);
    m_records.push_back(std::move(record));
    return true;
}
}
