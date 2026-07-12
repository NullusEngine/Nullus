#include "Assets/EditorAssetDragDropBridge.h"

#include "Assets/AssetMeta.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/AssetImporterFacade.h"
#include "Assets/ArtifactDatabaseManifestUtils.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/ExternalAssetImporter.h"
#include "Assets/ImportedPrefabRendererDependencyTemplates.h"
#include "Assets/ModelTextureReferenceResolver.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/SourceFileHashCache.h"
#include "Core/ServiceLocator.h"
#include "Debug/Logger.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Serialize/ObjectGraphBinaryCache.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphWriter.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <Json/json.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace NLS::Editor::Assets
{
namespace
{
struct FastImportedPrefabLoadResult
{
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> prefab;
    std::optional<UnifiedPrefabLoadKey> key;
    std::shared_ptr<const std::vector<ImportedPrefabRendererDependencyTemplate>> rendererDependencyTemplates;
    size_t prefabArtifactBytes = 0u;
    bool rendererDependencyMissing = false;
    std::string diagnosticCode;
    std::string diagnosticMessage;
};

enum class ImportedPrefabArtifactLoadMode
{
    RequireRendererArtifactFiles,
    PreviewGraphOnly
};

bool IncludeRendererArtifactStamp(const ImportedPrefabArtifactLoadMode loadMode)
{
    return loadMode != ImportedPrefabArtifactLoadMode::PreviewGraphOnly;
}

struct ImportedAssetHandle
{
    std::string assetPath;
    std::string prefabSubAssetKey;
    NLS::Core::Assets::AssetType assetType = NLS::Core::Assets::AssetType::Unknown;
    NLS::Core::Assets::AssetId assetId;
};

struct ImportedPrefabHotCacheEntry
{
    UnifiedPrefabLoadKey key;
    FastImportedPrefabLoadResult result;
    size_t estimatedBytes = 0u;
    uint64_t lastUsed = 0u;
};

struct ImportedPrefabHotCache
{
    std::vector<ImportedPrefabHotCacheEntry> entries;
    size_t retainedBytes = 0u;
    uint64_t useCounter = 0u;
};

struct ModelTextureMappingDependencyFingerprintCacheEntry
{
    std::string key;
    std::string fingerprint;
    uint64_t lastUsed = 0u;
};

struct ModelTextureMappingArtifactDatabaseLookupIndex
{
    std::vector<ModelTextureAssetCandidate> candidates;
    std::unordered_map<std::string, std::vector<size_t>> sourcePathCandidates;
    std::unordered_map<std::string, std::vector<size_t>> nameTokenCandidates;
};

struct ModelTextureMappingArtifactDatabaseTextureIndexCacheEntry
{
    std::string key;
    std::shared_ptr<const ModelTextureMappingArtifactDatabaseLookupIndex> index;
    uint64_t lastUsed = 0u;
};

struct ModelTextureMappingDependencyFingerprintResult
{
    std::string fingerprint;
    bool cacheable = false;
};

constexpr size_t kMaxImportedPrefabHotCacheEntries = 16u;
constexpr size_t kMaxImportedPrefabHotCacheBytes = 64u * 1024u * 1024u;
constexpr uint32_t kPersistentPreparedPrefabCacheSchemaVersion = 7u;
constexpr uint32_t kPreparedPrefabReflectionSchemaVersion = 1u;
constexpr uint32_t kPreparedPrefabSerializationFormatVersion = 1u;
constexpr uint32_t kPreparedPrefabDependencyManifestVersion = 1u;
constexpr size_t kMaxModelTextureMappingDependencyFingerprintCacheEntries = 512u;
constexpr size_t kMaxModelTextureMappingArtifactDatabaseTextureIndexCacheEntries = 8u;
constexpr uint32_t kPersistentModelTextureMappingArtifactDatabaseTextureIndexSchemaVersion = 1u;
constexpr uintmax_t kMaxPersistentModelTextureMappingArtifactDatabaseTextureIndexCacheBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxPersistentPreparedPrefabCacheEntries = 512u;
constexpr uintmax_t kMaxPersistentPreparedPrefabCacheBytes = 256u * 1024u * 1024u;
constexpr size_t kPersistentPreparedPrefabCachePruneWriteInterval = 16u;

class ScopedBlockingPrefabDropProgress
{
public:
    ScopedBlockingPrefabDropProgress(
        ImportProgressTracker* tracker,
        NLS::Core::Assets::AssetId assetId,
        std::string sourcePath)
        : m_tracker(tracker)
    {
        if (m_tracker == nullptr)
            return;

        m_job = m_tracker->BeginJob(assetId, std::move(sourcePath), "editor", 1u);
        Report(ImportPhase::Queued, 0.01, "Preparing prefab load");
    }

    ~ScopedBlockingPrefabDropProgress()
    {
        if (IsRunning())
            Finish(ImportJobTerminalStatus::Failed);
    }

    ScopedBlockingPrefabDropProgress(const ScopedBlockingPrefabDropProgress&) = delete;
    ScopedBlockingPrefabDropProgress& operator=(const ScopedBlockingPrefabDropProgress&) = delete;

    void Report(const ImportPhase phase, const double progress, std::string message)
    {
        if (!IsRunning())
            return;
        m_tracker->ReportProgress(m_job, phase, progress, std::move(message));
    }

    void Finish(const ImportJobTerminalStatus status)
    {
        if (!IsRunning())
            return;
        m_tracker->FinishJob(m_job, status, {});
        m_finished = true;
    }

    void FinishForDragDropResult(const AssetDragDropResult& result)
    {
        Finish(result.status == DragDropOperationStatus::Committed
            ? ImportJobTerminalStatus::Succeeded
            : ImportJobTerminalStatus::Failed);
    }

    void FinishForPendingHandoff()
    {
        Finish(ImportJobTerminalStatus::Succeeded);
    }

private:
    bool IsRunning() const
    {
        return m_tracker != nullptr && m_job.IsValid() && !m_finished;
    }

    ImportProgressTracker* m_tracker = nullptr;
    ImportJobId m_job;
    bool m_finished = false;
};

std::string DefaultGeneratedPrefabSubAssetKeyForAssetPath(const std::string& assetPath)
{
    return "prefab:" + std::filesystem::path(assetPath).stem().generic_string();
}

std::string NormalizeProjectAssetPath(const std::string& assetPath)
{
    if (assetPath.empty() || assetPath.front() == ':')
        return {};

    auto normalized = NormalizeEditorAssetPath(assetPath);
    if (normalized == "Assets" || normalized.rfind("Assets/", 0u) == 0u)
        return normalized;
    return NormalizeEditorAssetPath(std::filesystem::path("Assets") / normalized);
}

std::string FileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return {};

    const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
    return std::to_string(size) + ":" + std::to_string(writeTimeTicks);
}

std::optional<NLS::Core::Assets::ArtifactManifest> LoadArtifactManifestForSource(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId sourceAssetId,
    const std::string& targetPlatform = "editor")
{
    return LoadArtifactManifestFromProjectArtifactDB(projectRoot, sourceAssetId, targetPlatform);
}

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool CaseInsensitiveMatch(const std::string& lhs, const std::string& rhs)
{
    return ToLower(lhs) == ToLower(rhs);
}

std::mutex& ModelTextureMappingDependencyFingerprintCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<ModelTextureMappingDependencyFingerprintCacheEntry>& ModelTextureMappingDependencyFingerprintCache()
{
    static std::vector<ModelTextureMappingDependencyFingerprintCacheEntry> cache;
    return cache;
}

std::vector<ModelTextureMappingArtifactDatabaseTextureIndexCacheEntry>&
ModelTextureMappingArtifactDatabaseTextureIndexCache()
{
    static std::vector<ModelTextureMappingArtifactDatabaseTextureIndexCacheEntry> cache;
    return cache;
}

uint64_t& ModelTextureMappingDependencyFingerprintCacheUseCounter()
{
    static uint64_t counter = 0u;
    return counter;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
size_t& ModelTextureMappingDependencyFingerprintScanCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

size_t& ModelTextureMappingDependencyArtifactDatabaseLoadCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

size_t& ModelTextureMappingDependencyMetaLoadCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}

size_t& ModelTextureMappingDependencySourcePathFallbackCountForTestingStorage()
{
    static size_t count = 0u;
    return count;
}
#endif

bool ParseModelTextureMappingDependencyValue(
    const std::string& value,
    std::string& query,
    std::string& mode);

std::string BuildModelTextureMappingDependencyProjectStamp(
    const std::filesystem::path& projectRoot,
    const std::string& dependencyValue,
    const std::string& artifactDatabaseDataStamp)
{
    std::string query;
    std::string mode;
    (void)ParseModelTextureMappingDependencyValue(dependencyValue, query, mode);

    std::string stamp = "artifactDb@" + artifactDatabaseDataStamp;
    if (mode == "source-path")
    {
        const auto sourcePath = (projectRoot / NormalizeEditorAssetPath(query)).lexically_normal();
        const auto metaPath = NLS::Core::Assets::GetAssetMetaPath(sourcePath);
        stamp += "|source@" + NormalizeEditorAssetPath(query) + "@" + FileStamp(sourcePath);
        stamp += "|meta@" + FileStamp(metaPath);
    }
    return stamp;
}

std::string BuildModelTextureMappingDependencyProjectStamp(
    const std::filesystem::path& projectRoot,
    const std::string& dependencyValue)
{
    const auto artifactDatabasePath = GetProjectArtifactDatabasePath(projectRoot);
    return BuildModelTextureMappingDependencyProjectStamp(
        projectRoot,
        dependencyValue,
        GetArtifactDatabaseDataFileStamp(artifactDatabasePath));
}

std::string BuildModelTextureMappingDependencyFingerprintCacheKey(
    const std::filesystem::path& projectRoot,
    const std::string& dependencyValue,
    const std::string& targetPlatform,
    const std::string& artifactDatabaseDataStamp)
{
    return projectRoot.lexically_normal().generic_string() +
        "|" + dependencyValue +
        "|" + targetPlatform +
        "|" + BuildModelTextureMappingDependencyProjectStamp(
            projectRoot,
            dependencyValue,
            artifactDatabaseDataStamp);
}

std::string BuildModelTextureMappingDependencyFingerprintCacheKey(
    const std::filesystem::path& projectRoot,
    const std::string& dependencyValue,
    const std::string& targetPlatform)
{
    const auto artifactDatabasePath = GetProjectArtifactDatabasePath(projectRoot);
    return BuildModelTextureMappingDependencyFingerprintCacheKey(
        projectRoot,
        dependencyValue,
        targetPlatform,
        GetArtifactDatabaseDataFileStamp(artifactDatabasePath));
}

std::optional<std::string> TryGetCachedModelTextureMappingDependencyFingerprint(
    const std::string& cacheKey)
{
    std::lock_guard lock(ModelTextureMappingDependencyFingerprintCacheMutex());
    auto& cache = ModelTextureMappingDependencyFingerprintCache();
    auto& useCounter = ModelTextureMappingDependencyFingerprintCacheUseCounter();
    const auto found = std::find_if(
        cache.begin(),
        cache.end(),
        [&cacheKey](const ModelTextureMappingDependencyFingerprintCacheEntry& entry)
        {
            return entry.key == cacheKey;
        });
    if (found == cache.end())
        return std::nullopt;

    found->lastUsed = ++useCounter;
    return found->fingerprint;
}

void StoreModelTextureMappingDependencyFingerprint(
    std::string cacheKey,
    std::string fingerprint)
{
    std::lock_guard lock(ModelTextureMappingDependencyFingerprintCacheMutex());
    auto& cache = ModelTextureMappingDependencyFingerprintCache();
    auto& useCounter = ModelTextureMappingDependencyFingerprintCacheUseCounter();
    const auto found = std::find_if(
        cache.begin(),
        cache.end(),
        [&cacheKey](const ModelTextureMappingDependencyFingerprintCacheEntry& entry)
        {
            return entry.key == cacheKey;
        });
    if (found != cache.end())
    {
        found->fingerprint = std::move(fingerprint);
        found->lastUsed = ++useCounter;
        return;
    }

    cache.push_back({std::move(cacheKey), std::move(fingerprint), ++useCounter});
    while (cache.size() > kMaxModelTextureMappingDependencyFingerprintCacheEntries)
    {
        const auto oldest = std::min_element(
            cache.begin(),
            cache.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.lastUsed < rhs.lastUsed;
            });
        if (oldest == cache.end())
            break;
        cache.erase(oldest);
    }
}

std::string BuildModelTextureMappingArtifactDatabaseTextureIndexCacheKey(
    const std::filesystem::path& projectRoot,
    const std::string& targetPlatform,
    const std::string& artifactDatabaseDataStamp)
{
    return projectRoot.lexically_normal().generic_string() +
        "|" +
        targetPlatform +
        "|artifactDb@" +
        artifactDatabaseDataStamp;
}

std::shared_ptr<const ModelTextureMappingArtifactDatabaseLookupIndex>
TryGetCachedModelTextureMappingArtifactDatabaseTextureIndex(const std::string& cacheKey)
{
    std::lock_guard lock(ModelTextureMappingDependencyFingerprintCacheMutex());
    auto& cache = ModelTextureMappingArtifactDatabaseTextureIndexCache();
    auto& useCounter = ModelTextureMappingDependencyFingerprintCacheUseCounter();
    const auto found = std::find_if(
        cache.begin(),
        cache.end(),
        [&cacheKey](const ModelTextureMappingArtifactDatabaseTextureIndexCacheEntry& entry)
        {
            return entry.key == cacheKey;
        });
    if (found == cache.end())
        return nullptr;

    found->lastUsed = ++useCounter;
    return found->index;
}

void StoreModelTextureMappingArtifactDatabaseTextureIndex(
    std::string cacheKey,
    std::shared_ptr<const ModelTextureMappingArtifactDatabaseLookupIndex> index)
{
    if (index == nullptr)
        return;

    std::lock_guard lock(ModelTextureMappingDependencyFingerprintCacheMutex());
    auto& cache = ModelTextureMappingArtifactDatabaseTextureIndexCache();
    auto& useCounter = ModelTextureMappingDependencyFingerprintCacheUseCounter();
    const auto found = std::find_if(
        cache.begin(),
        cache.end(),
        [&cacheKey](const ModelTextureMappingArtifactDatabaseTextureIndexCacheEntry& entry)
        {
            return entry.key == cacheKey;
        });
    if (found != cache.end())
    {
        found->index = std::move(index);
        found->lastUsed = ++useCounter;
        return;
    }

    cache.push_back({std::move(cacheKey), std::move(index), ++useCounter});
    while (cache.size() > kMaxModelTextureMappingArtifactDatabaseTextureIndexCacheEntries)
    {
        const auto oldest = std::min_element(
            cache.begin(),
            cache.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.lastUsed < rhs.lastUsed;
            });
        if (oldest == cache.end())
            break;
        cache.erase(oldest);
    }
}

std::filesystem::path ModelTextureMappingArtifactDatabaseTextureIndexCachePath(
    const std::filesystem::path& projectRoot)
{
    return projectRoot / "Library" / "ModelTextureMappingArtifactDatabaseTextureIndex.cache";
}

std::optional<std::string> JsonStringValue(
    const nlohmann::json& object,
    const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string())
        return std::nullopt;
    return found->get<std::string>();
}

bool WriteJsonAtomically(
    const std::filesystem::path& path,
    const nlohmann::json& root)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    const auto tempPath = path.string() + ".tmp";
    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
            return false;
        output << root.dump(1);
        if (!output.good())
            return false;
    }

    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(tempPath, path, error);
    if (error)
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }
    return true;
}

std::shared_ptr<const ModelTextureMappingArtifactDatabaseLookupIndex> BuildModelTextureMappingArtifactDatabaseLookupIndex(
    std::vector<ModelTextureAssetCandidate> candidates);

std::shared_ptr<const ModelTextureMappingArtifactDatabaseLookupIndex>
TryLoadPersistentModelTextureMappingArtifactDatabaseTextureIndex(
    const std::filesystem::path& projectRoot,
    const std::string& targetPlatform,
    const std::string& artifactDatabaseDataStamp)
{
    if (artifactDatabaseDataStamp.empty())
        return nullptr;

    try
    {
        const auto cachePath = ModelTextureMappingArtifactDatabaseTextureIndexCachePath(projectRoot);
        std::error_code error;
        const auto cacheSize = std::filesystem::file_size(cachePath, error);
        if (error || cacheSize > kMaxPersistentModelTextureMappingArtifactDatabaseTextureIndexCacheBytes)
            return nullptr;

        std::ifstream input(
            cachePath,
            std::ios::binary);
        if (!input.is_open())
            return nullptr;

        const auto root = nlohmann::json::parse(input, nullptr, false);
        if (root.is_discarded() || !root.is_object())
            return nullptr;
        if (root.value("schemaVersion", 0u) !=
            kPersistentModelTextureMappingArtifactDatabaseTextureIndexSchemaVersion)
        {
            return nullptr;
        }
        if (root.value("targetPlatform", std::string()) != targetPlatform ||
            root.value("artifactDatabaseDataStamp", std::string()) != artifactDatabaseDataStamp)
        {
            return nullptr;
        }

        const auto entries = root.find("candidates");
        if (entries == root.end() || !entries->is_array())
            return nullptr;

        std::vector<ModelTextureAssetCandidate> candidates;
        candidates.reserve(entries->size());
        for (const auto& item : *entries)
        {
            if (!item.is_object())
                return nullptr;

            const auto assetIdText = JsonStringValue(item, "assetId");
            const auto editorPath = JsonStringValue(item, "editorPath");
            if (!assetIdText.has_value() || !editorPath.has_value())
                return nullptr;

            const auto guid = NLS::Guid::TryParse(*assetIdText);
            if (!guid.has_value() || !guid->IsValid())
                return nullptr;

            ModelTextureAssetCandidate candidate;
            candidate.assetId = NLS::Core::Assets::AssetId(*guid);
            candidate.subAssetKey = item.value("subAssetKey", std::string());
            candidate.editorPath = *editorPath;
            candidate.artifactPath = item.value("artifactPath", std::string());
            candidate.displayName = item.value("displayName", std::string());
            candidate.assetType = NLS::Core::Assets::AssetType::Texture;
            candidate.imported = item.value("imported", true);
            candidate.rootIndex = item.value("rootIndex", 0u);
            candidate.artifactHashOrVersion = item.value("artifactHashOrVersion", std::string());
            candidates.push_back(std::move(candidate));
        }

        return BuildModelTextureMappingArtifactDatabaseLookupIndex(std::move(candidates));
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

void StorePersistentModelTextureMappingArtifactDatabaseTextureIndex(
    const std::filesystem::path& projectRoot,
    const std::string& targetPlatform,
    const std::string& artifactDatabaseDataStamp,
    const ModelTextureMappingArtifactDatabaseLookupIndex& index)
{
    if (artifactDatabaseDataStamp.empty())
        return;

    nlohmann::json root = nlohmann::json::object();
    root["schemaVersion"] = kPersistentModelTextureMappingArtifactDatabaseTextureIndexSchemaVersion;
    root["targetPlatform"] = targetPlatform;
    root["artifactDatabaseDataStamp"] = artifactDatabaseDataStamp;
    root["candidates"] = nlohmann::json::array();
    for (const auto& candidate : index.candidates)
    {
        nlohmann::json item = nlohmann::json::object();
        item["assetId"] = candidate.assetId.ToString();
        item["subAssetKey"] = candidate.subAssetKey;
        item["editorPath"] = candidate.editorPath.lexically_normal().generic_string();
        item["artifactPath"] = candidate.artifactPath.lexically_normal().generic_string();
        item["displayName"] = candidate.displayName;
        item["imported"] = candidate.imported;
        item["rootIndex"] = candidate.rootIndex;
        item["artifactHashOrVersion"] = candidate.artifactHashOrVersion;
        root["candidates"].push_back(std::move(item));
    }

    (void)WriteJsonAtomically(
        ModelTextureMappingArtifactDatabaseTextureIndexCachePath(projectRoot),
        root);
}

bool ParseModelTextureMappingDependencyValue(
    const std::string& value,
    std::string& query,
    std::string& mode)
{
    constexpr std::string_view prefix = "project|";
    if (value.rfind(prefix.data(), 0u) != 0u)
        return false;

    const auto payload = value.substr(prefix.size());
    const auto separator = payload.rfind('|');
    if (separator == std::string::npos)
        return false;

    query = payload.substr(0u, separator);
    mode = payload.substr(separator + 1u);
    return !query.empty() && (mode == "source-path" || mode == "name-search");
}

std::string NormalizeModelTextureMappingLookupKey(const std::filesystem::path& path)
{
    return ToLower(NormalizeEditorAssetPath(path));
}

void AddModelTextureMappingLookupEntry(
    std::unordered_map<std::string, std::vector<size_t>>& lookup,
    const std::string& key,
    const size_t candidateIndex)
{
    if (key.empty())
        return;

    auto& indices = lookup[key];
    if (std::find(indices.begin(), indices.end(), candidateIndex) == indices.end())
        indices.push_back(candidateIndex);
}

std::shared_ptr<const ModelTextureMappingArtifactDatabaseLookupIndex> BuildModelTextureMappingArtifactDatabaseLookupIndex(
    std::vector<ModelTextureAssetCandidate> candidates)
{
    ModelTextureMappingArtifactDatabaseLookupIndex index;
    index.candidates = std::move(candidates);
    for (size_t candidateIndex = 0u; candidateIndex < index.candidates.size(); ++candidateIndex)
    {
        const auto& candidate = index.candidates[candidateIndex];
        AddModelTextureMappingLookupEntry(
            index.sourcePathCandidates,
            NormalizeModelTextureMappingLookupKey(candidate.editorPath),
            candidateIndex);

        const auto path = std::filesystem::path(candidate.editorPath);
        AddModelTextureMappingLookupEntry(
            index.nameTokenCandidates,
            NormalizeModelTextureMappingLookupKey(path.filename()),
            candidateIndex);
        AddModelTextureMappingLookupEntry(
            index.nameTokenCandidates,
            NormalizeModelTextureMappingLookupKey(path.stem()),
            candidateIndex);
    }
    return std::make_shared<const ModelTextureMappingArtifactDatabaseLookupIndex>(std::move(index));
}

std::optional<ModelTextureAssetCandidate> BuildModelTextureMappingCandidateForEditorPath(
    const std::filesystem::path& projectRoot,
    const std::string& editorPath,
    const std::string& targetPlatform,
    const std::string& nameQuery)
{
    const auto normalizedEditorPath = NormalizeEditorAssetPath(editorPath);
    const auto absolutePath = (projectRoot / normalizedEditorPath).lexically_normal();
#if defined(NLS_ENABLE_TEST_HOOKS)
    ++ModelTextureMappingDependencyMetaLoadCountForTestingStorage();
#endif
    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(absolutePath));
    if (!meta.has_value() ||
        !meta->id.IsValid() ||
        meta->assetType != NLS::Core::Assets::AssetType::Texture)
    {
        return std::nullopt;
    }

    ModelTextureAssetCandidate candidate;
    candidate.assetId = meta->id;
    candidate.editorPath = normalizedEditorPath;
    candidate.displayName = absolutePath.stem().generic_string();
    candidate.assetType = NLS::Core::Assets::AssetType::Texture;
    candidate.imported = false;
    candidate.rootIndex = 0u;
    candidate.nameQuery = nameQuery;

    const auto manifest = LoadArtifactManifestForSource(projectRoot, meta->id);
    if (manifest.has_value())
    {
        const auto exactTextureArtifact = std::find_if(
            manifest->subAssets.begin(),
            manifest->subAssets.end(),
            [&targetPlatform](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture &&
                    artifact.targetPlatform == targetPlatform;
            });
        auto textureArtifact = exactTextureArtifact;
        if (textureArtifact == manifest->subAssets.end())
        {
            textureArtifact = std::find_if(
                manifest->subAssets.begin(),
                manifest->subAssets.end(),
                [](const NLS::Core::Assets::ImportedArtifact& artifact)
                {
                    return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
                });
        }
        if (textureArtifact != manifest->subAssets.end())
        {
            candidate.subAssetKey = textureArtifact->subAssetKey;
            candidate.artifactPath = textureArtifact->artifactPath;
            candidate.displayName = textureArtifact->displayName.empty()
                ? candidate.displayName
                : textureArtifact->displayName;
            candidate.imported = true;
            candidate.artifactHashOrVersion = textureArtifact->contentHash;
        }
    }

    return candidate;
}

std::vector<ModelTextureAssetCandidate> BuildModelTextureMappingProjectTextureIndex(
    const std::filesystem::path& projectRoot,
    const std::string& targetPlatform)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ++ModelTextureMappingDependencyFingerprintScanCountForTestingStorage();
#endif
    std::vector<ModelTextureAssetCandidate> candidates;
    const auto assetsRoot = (projectRoot / "Assets").lexically_normal();
    std::error_code error;
    if (!std::filesystem::is_directory(assetsRoot, error))
        return candidates;

    for (std::filesystem::recursive_directory_iterator iterator(
             assetsRoot,
             std::filesystem::directory_options::skip_permission_denied,
             error);
         !error && iterator != std::filesystem::recursive_directory_iterator();
         iterator.increment(error))
    {
        if (!iterator->is_regular_file(error) || iterator->path().extension() == ".meta")
            continue;

        const auto editorPath = NormalizeEditorAssetPath(iterator->path().lexically_relative(projectRoot));
        if (auto candidate = BuildModelTextureMappingCandidateForEditorPath(
                projectRoot,
                editorPath,
                targetPlatform,
                {});
            candidate.has_value())
        {
            candidates.push_back(std::move(*candidate));
        }
    }
    return candidates;
}

std::shared_ptr<const ModelTextureMappingArtifactDatabaseLookupIndex> BuildModelTextureMappingArtifactDatabaseTextureIndex(
    const std::filesystem::path& projectRoot,
    const std::string& targetPlatform)
{
    const auto artifactDatabasePath = GetProjectArtifactDatabasePath(projectRoot);
    const auto artifactDatabaseDataStamp = GetArtifactDatabaseDataFileStamp(artifactDatabasePath);
    std::string cacheKey;
    if (!artifactDatabaseDataStamp.empty())
    {
        cacheKey = BuildModelTextureMappingArtifactDatabaseTextureIndexCacheKey(
            projectRoot,
            targetPlatform,
            artifactDatabaseDataStamp);
        if (auto cached = TryGetCachedModelTextureMappingArtifactDatabaseTextureIndex(cacheKey))
            return cached;
        if (auto persistentCached = TryLoadPersistentModelTextureMappingArtifactDatabaseTextureIndex(
                projectRoot,
                targetPlatform,
                artifactDatabaseDataStamp))
        {
            StoreModelTextureMappingArtifactDatabaseTextureIndex(cacheKey, persistentCached);
            return persistentCached;
        }
    }

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    if (!artifactDatabase.Load(artifactDatabasePath))
        return nullptr;
#if defined(NLS_ENABLE_TEST_HOOKS)
    ++ModelTextureMappingDependencyArtifactDatabaseLoadCountForTestingStorage();
#endif

    std::unordered_map<std::string, const NLS::Core::Assets::ArtifactDatabaseRecord*> selectedTextureRecords;
    for (const auto& record : artifactDatabase.GetRecords())
    {
        if (record.artifactType != NLS::Core::Assets::ArtifactType::Texture ||
            record.status != NLS::Core::Assets::ArtifactRecordStatus::UpToDate)
        {
            continue;
        }

        const auto selectionKey = record.sourceAssetId.ToString() + "\n" + record.subAssetKey;
        const auto selected = selectedTextureRecords.find(selectionKey);
        if (selected != selectedTextureRecords.end() &&
            selected->second->targetPlatform == targetPlatform)
        {
            continue;
        }
        if (selected == selectedTextureRecords.end() || record.targetPlatform == targetPlatform)
            selectedTextureRecords[selectionKey] = &record;
    }

    std::vector<ModelTextureAssetCandidate> candidates;
    candidates.reserve(selectedTextureRecords.size());
    for (const auto& [_, record] : selectedTextureRecords)
    {
        ModelTextureAssetCandidate candidate;
        candidate.assetId = record->sourceAssetId;
        candidate.editorPath = NormalizeEditorAssetPath(record->sourcePath);
        candidate.subAssetKey = record->subAssetKey;
        candidate.artifactPath = record->artifactPath;
        candidate.displayName = record->displayName.empty()
            ? std::filesystem::path(record->sourcePath).stem().generic_string()
            : record->displayName;
        candidate.assetType = NLS::Core::Assets::AssetType::Texture;
        candidate.imported = true;
        candidate.rootIndex = 0u;
        candidate.artifactHashOrVersion = record->contentHash;
        candidates.push_back(std::move(candidate));
    }
    auto index = BuildModelTextureMappingArtifactDatabaseLookupIndex(std::move(candidates));
    if (!cacheKey.empty())
    {
        StoreModelTextureMappingArtifactDatabaseTextureIndex(cacheKey, index);
        StorePersistentModelTextureMappingArtifactDatabaseTextureIndex(
            projectRoot,
            targetPlatform,
            artifactDatabaseDataStamp,
            *index);
    }
    return index;
}

std::optional<std::string> ComputeModelTextureMappingDependencyFingerprintFromArtifactDatabaseIndex(
    const std::vector<ModelTextureAssetCandidate>& artifactDatabaseIndex,
    const std::string& query,
    const std::string& mode)
{
    std::vector<ModelTextureAssetCandidate> candidates;
    for (auto candidate : artifactDatabaseIndex)
    {
        if (mode == "source-path")
        {
            if (!CaseInsensitiveMatch(
                    NormalizeEditorAssetPath(candidate.editorPath),
                    NormalizeEditorAssetPath(query)))
            {
                continue;
            }
        }
        else
        {
            const auto path = std::filesystem::path(candidate.editorPath);
            if (!CaseInsensitiveMatch(NormalizeEditorAssetPath(path.filename()), query) &&
                !CaseInsensitiveMatch(NormalizeEditorAssetPath(path.stem()), query))
            {
                continue;
            }
            candidate.nameQuery = query;
        }

        candidates.push_back(std::move(candidate));
    }

    if (candidates.empty())
        return std::nullopt;
    return BuildModelTextureMappingFingerprint(candidates);
}

std::optional<std::string> ComputeModelTextureMappingDependencyFingerprintFromArtifactDatabaseLookupIndex(
    const ModelTextureMappingArtifactDatabaseLookupIndex& index,
    const std::string& query,
    const std::string& mode,
    size_t* candidateScanCount)
{
    if (index.candidates.empty())
        return std::nullopt;

    const auto lookupKey = NormalizeModelTextureMappingLookupKey(query);
    const auto& lookup = mode == "source-path"
        ? index.sourcePathCandidates
        : index.nameTokenCandidates;
    const auto found = lookup.find(lookupKey);
    if (found == lookup.end())
        return std::nullopt;

    std::vector<ModelTextureAssetCandidate> candidates;
    candidates.reserve(found->second.size());
    if (candidateScanCount != nullptr)
        *candidateScanCount += found->second.size();
    for (const auto candidateIndex : found->second)
    {
        if (candidateIndex >= index.candidates.size())
            continue;

        auto candidate = index.candidates[candidateIndex];
        if (mode != "source-path")
            candidate.nameQuery = query;
        candidates.push_back(std::move(candidate));
    }

    if (candidates.empty())
        return std::nullopt;
    return BuildModelTextureMappingFingerprint(candidates);
}

std::optional<std::string> ComputeModelTextureMappingDependencyFingerprintFromArtifactDatabase(
    const std::filesystem::path& projectRoot,
    const std::string& query,
    const std::string& mode,
    const std::string& targetPlatform)
{
    auto artifactDatabaseIndex = BuildModelTextureMappingArtifactDatabaseTextureIndex(
        projectRoot,
        targetPlatform);
    if (!artifactDatabaseIndex)
        return std::nullopt;
    return ComputeModelTextureMappingDependencyFingerprintFromArtifactDatabaseLookupIndex(
        *artifactDatabaseIndex,
        query,
        mode,
        nullptr);
}

std::string ComputeModelTextureMappingDependencyFingerprintFromSourcePathFallback(
    const std::filesystem::path& projectRoot,
    const std::string& query,
    const std::string& targetPlatform);

std::optional<ModelTextureMappingDependencyFingerprintResult> ComputeModelTextureMappingDependencyFingerprintUncached(
    const std::filesystem::path& projectRoot,
    const std::string& query,
    const std::string& mode,
    const std::string& targetPlatform,
    const std::vector<ModelTextureAssetCandidate>* projectTextureIndex)
{
    if (auto fromArtifactDatabase = ComputeModelTextureMappingDependencyFingerprintFromArtifactDatabase(
            projectRoot,
            query,
            mode,
        targetPlatform);
        fromArtifactDatabase.has_value())
    {
        return ModelTextureMappingDependencyFingerprintResult {std::move(*fromArtifactDatabase), true};
    }

    if (mode == "source-path")
    {
#if defined(NLS_ENABLE_TEST_HOOKS)
        ++ModelTextureMappingDependencySourcePathFallbackCountForTestingStorage();
#endif
        return ModelTextureMappingDependencyFingerprintResult {
            ComputeModelTextureMappingDependencyFingerprintFromSourcePathFallback(projectRoot, query, targetPlatform),
            true};
    }

    std::vector<ModelTextureAssetCandidate> localProjectIndex;
    if (projectTextureIndex == nullptr)
        localProjectIndex = BuildModelTextureMappingProjectTextureIndex(projectRoot, targetPlatform);
    const auto& index = projectTextureIndex != nullptr ? *projectTextureIndex : localProjectIndex;
    std::vector<ModelTextureAssetCandidate> candidates;
    for (auto candidate : index)
    {
        const auto path = std::filesystem::path(candidate.editorPath);
        if (!CaseInsensitiveMatch(NormalizeEditorAssetPath(path.filename()), query) &&
            !CaseInsensitiveMatch(NormalizeEditorAssetPath(path.stem()), query))
        {
            continue;
        }
        candidate.nameQuery = query;
        candidates.push_back(std::move(candidate));
    }
    return ModelTextureMappingDependencyFingerprintResult {BuildModelTextureMappingFingerprint(candidates), false};
}

std::optional<std::string> ComputeModelTextureMappingDependencyFingerprintFromFallbackIndex(
    const std::string& query,
    const std::vector<ModelTextureAssetCandidate>& projectTextureIndex)
{
    std::vector<ModelTextureAssetCandidate> candidates;
    for (auto candidate : projectTextureIndex)
    {
        const auto path = std::filesystem::path(candidate.editorPath);
        if (!CaseInsensitiveMatch(NormalizeEditorAssetPath(path.filename()), query) &&
            !CaseInsensitiveMatch(NormalizeEditorAssetPath(path.stem()), query))
        {
            continue;
        }
        candidate.nameQuery = query;
        candidates.push_back(std::move(candidate));
    }
    return BuildModelTextureMappingFingerprint(candidates);
}

std::string ComputeModelTextureMappingDependencyFingerprintFromSourcePathFallback(
    const std::filesystem::path& projectRoot,
    const std::string& query,
    const std::string& targetPlatform)
{
    std::vector<ModelTextureAssetCandidate> candidates;
    if (auto candidate = BuildModelTextureMappingCandidateForEditorPath(
            projectRoot,
            query,
            targetPlatform,
            {});
        candidate.has_value())
    {
        candidates.push_back(std::move(*candidate));
    }
    return BuildModelTextureMappingFingerprint(candidates);
}

std::optional<std::string> ComputeModelTextureMappingDependencyFingerprint(
    const std::filesystem::path& projectRoot,
    const std::string& dependencyValue,
    const std::string& targetPlatform)
{
    std::string query;
    std::string mode;
    if (!ParseModelTextureMappingDependencyValue(dependencyValue, query, mode))
        return std::nullopt;

    const auto cacheKey = BuildModelTextureMappingDependencyFingerprintCacheKey(
        projectRoot,
        dependencyValue,
        targetPlatform);
    if (auto cached = TryGetCachedModelTextureMappingDependencyFingerprint(cacheKey); cached.has_value())
        return cached;

    auto fingerprint = ComputeModelTextureMappingDependencyFingerprintUncached(
        projectRoot,
        query,
        mode,
        targetPlatform,
        nullptr);
    if (fingerprint.has_value())
    {
        if (fingerprint->cacheable)
            StoreModelTextureMappingDependencyFingerprint(cacheKey, fingerprint->fingerprint);
        return fingerprint->fingerprint;
    }
    return std::nullopt;
}

std::vector<std::optional<std::string>> ComputeModelTextureMappingDependencyFingerprints(
    const std::filesystem::path& projectRoot,
    const std::vector<std::string>& dependencyValues,
    const std::string& targetPlatform)
{
    NLS::Base::Profiling::PerformanceStageScope batchScope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        "ComputeModelTextureMappingDependencyFingerprintBatch",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    std::vector<std::optional<std::string>> fingerprints;
    fingerprints.reserve(dependencyValues.size());
    std::optional<std::vector<ModelTextureAssetCandidate>> projectTextureIndex;
    std::shared_ptr<const ModelTextureMappingArtifactDatabaseLookupIndex> artifactDatabaseLookupIndex;
    bool triedArtifactDatabaseTextureIndex = false;
    const auto artifactDatabasePath = GetProjectArtifactDatabasePath(projectRoot);
    const auto artifactDatabaseDataStamp = GetArtifactDatabaseDataFileStamp(artifactDatabasePath);
    size_t invalidDependencyCount = 0u;
    size_t cacheHitCount = 0u;
    size_t cacheMissCount = 0u;
    size_t fingerprintCacheKeyBuildCount = 0u;
    size_t artifactDatabaseHitCount = 0u;
    size_t artifactDatabaseMissCount = 0u;
    size_t artifactDatabaseCandidateScanCount = 0u;
    size_t nameSearchFallbackCount = 0u;
    size_t sourcePathFallbackCount = 0u;
    size_t uncachedFallbackCount = 0u;
    for (const auto& dependencyValue : dependencyValues)
    {
        std::string query;
        std::string mode;
        if (!ParseModelTextureMappingDependencyValue(dependencyValue, query, mode))
        {
            ++invalidDependencyCount;
            fingerprints.push_back(std::nullopt);
            continue;
        }

        if (!triedArtifactDatabaseTextureIndex)
        {
            artifactDatabaseLookupIndex = BuildModelTextureMappingArtifactDatabaseTextureIndex(
                projectRoot,
                targetPlatform);
            triedArtifactDatabaseTextureIndex = true;
        }
        if (artifactDatabaseLookupIndex)
        {
            auto fromArtifactDatabase = ComputeModelTextureMappingDependencyFingerprintFromArtifactDatabaseLookupIndex(
                *artifactDatabaseLookupIndex,
                query,
                mode,
                &artifactDatabaseCandidateScanCount);
            if (fromArtifactDatabase.has_value())
            {
                ++artifactDatabaseHitCount;
                fingerprints.push_back(std::move(fromArtifactDatabase));
                continue;
            }
            ++artifactDatabaseMissCount;
        }

        std::string cacheKey;
        auto ensureCacheKey = [&]() -> const std::string&
        {
            if (cacheKey.empty())
            {
                cacheKey = BuildModelTextureMappingDependencyFingerprintCacheKey(
                    projectRoot,
                    dependencyValue,
                    targetPlatform,
                    artifactDatabaseDataStamp);
                ++fingerprintCacheKeyBuildCount;
            }
            return cacheKey;
        };
        if (auto cached = TryGetCachedModelTextureMappingDependencyFingerprint(ensureCacheKey()); cached.has_value())
        {
            ++cacheHitCount;
            fingerprints.push_back(cached);
            continue;
        }
        ++cacheMissCount;

        std::optional<std::string> fingerprint;
        if (mode == "name-search")
        {
            ++nameSearchFallbackCount;
            if (!projectTextureIndex.has_value())
                projectTextureIndex = BuildModelTextureMappingProjectTextureIndex(projectRoot, targetPlatform);
            fingerprint = ComputeModelTextureMappingDependencyFingerprintFromFallbackIndex(
                query,
                *projectTextureIndex);
        }
        else if (mode == "source-path")
        {
            ++sourcePathFallbackCount;
#if defined(NLS_ENABLE_TEST_HOOKS)
            ++ModelTextureMappingDependencySourcePathFallbackCountForTestingStorage();
#endif
            fingerprint = ComputeModelTextureMappingDependencyFingerprintFromSourcePathFallback(
                projectRoot,
                query,
                targetPlatform);
            if (fingerprint.has_value())
                StoreModelTextureMappingDependencyFingerprint(ensureCacheKey(), *fingerprint);
        }
        else
        {
            ++uncachedFallbackCount;
            auto uncachedFingerprint = ComputeModelTextureMappingDependencyFingerprintUncached(
                projectRoot,
                query,
                mode,
                targetPlatform,
                nullptr);
            if (uncachedFingerprint.has_value())
            {
                if (uncachedFingerprint->cacheable)
                    StoreModelTextureMappingDependencyFingerprint(ensureCacheKey(), uncachedFingerprint->fingerprint);
                fingerprint = std::move(uncachedFingerprint->fingerprint);
            }
        }
        fingerprints.push_back(std::move(fingerprint));
    }
    batchScope.AddCounter("dependencyCount", dependencyValues.size());
    batchScope.AddCounter("invalidDependencyCount", invalidDependencyCount);
    batchScope.AddCounter("cacheHitCount", cacheHitCount);
    batchScope.AddCounter("cacheMissCount", cacheMissCount);
    batchScope.AddCounter("fingerprintCacheKeyBuildCount", fingerprintCacheKeyBuildCount);
    batchScope.AddCounter("artifactDatabaseHitCount", artifactDatabaseHitCount);
    batchScope.AddCounter("artifactDatabaseMissCount", artifactDatabaseMissCount);
    batchScope.AddCounter("artifactDatabaseCandidateScanCount", artifactDatabaseCandidateScanCount);
    batchScope.AddCounter("nameSearchFallbackCount", nameSearchFallbackCount);
    batchScope.AddCounter("sourcePathFallbackCount", sourcePathFallbackCount);
    batchScope.AddCounter("uncachedFallbackCount", uncachedFallbackCount);
    return fingerprints;
}

uint64_t StableFnv1a64(const std::string& text)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : text)
    {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string Hex64(const uint64_t value)
{
    std::ostringstream stream;
    stream << std::hex << value;
    return stream.str();
}

std::string PreparedPrefabCacheBytesHash(const std::vector<uint8_t>& bytes)
{
    return NLS::Core::Assets::ComputeNativeArtifactPayloadHash(bytes.data(), bytes.size());
}

std::string PreparedPrefabCacheTextDigest(const std::string& text)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    return NLS::Core::Assets::ComputeNativeArtifactPayloadHash(bytes, text.size()) +
        ":bytes:" + std::to_string(text.size());
}

std::string PreparedPrefabCacheFilenameToken(std::string text)
{
    for (char& character : text)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '-' || character == '_')
            continue;
        character = '_';
    }
    return text.empty() ? std::string("empty") : std::move(text);
}

std::filesystem::path PersistentPreparedPrefabCachePath(
    const std::filesystem::path& projectRoot,
    const UnifiedPrefabLoadKey& key)
{
    return projectRoot /
        "Library" /
        "PreparedPrefabCache" /
        (Hex64(StableFnv1a64(key.runtimeCacheIdentity)) + ".json");
}

std::string PersistentPreparedPrefabCacheSidecarFilename(
    const std::filesystem::path& cachePath,
    const char* kind,
    const std::string& contentHash)
{
    return cachePath.stem().generic_string() +
        "." + kind +
        "." + PreparedPrefabCacheFilenameToken(contentHash) +
        ".objectgraphbin";
}

bool IsPreparedPrefabCacheSidecarExtension(const std::filesystem::path& path)
{
    return path.extension() == ".objectgraph" || path.extension() == ".objectgraphbin";
}

bool IsPreparedPrefabCacheSidecarFilenameSafe(const std::string& filename)
{
    if (filename.empty())
        return false;
    const std::filesystem::path path(filename);
    return path == path.filename() && !path.is_absolute();
}

std::optional<std::string> ReadPreparedPrefabCacheTextFile(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (!error && size <= static_cast<uintmax_t>(std::numeric_limits<size_t>::max()))
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return std::nullopt;

        std::string text;
        text.resize(static_cast<size_t>(size));
        if (!text.empty())
        {
            input.read(text.data(), static_cast<std::streamsize>(text.size()));
            if (input.gcount() != static_cast<std::streamsize>(text.size()))
                return std::nullopt;
        }
        return text;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

std::optional<std::vector<uint8_t>> ReadPreparedPrefabCacheBinaryFile(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (!error && size <= static_cast<uintmax_t>(std::numeric_limits<size_t>::max()))
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return std::nullopt;

        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!bytes.empty())
        {
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
                return std::nullopt;
        }
        return bytes;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool WritePreparedPrefabCacheTextFile(
    const std::filesystem::path& path,
    const std::string& text)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;

    const auto tempPath = path.string() + ".tmp";
    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        output << text;
    }

    error.clear();
    std::filesystem::rename(tempPath, path, error);
    if (error)
    {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(tempPath, path, error);
    }
    if (error)
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }
    return true;
}

bool WritePreparedPrefabCacheBinaryFile(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;

    const auto tempPath = path.string() + ".tmp";
    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        if (!bytes.empty())
        {
            output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
    }

    error.clear();
    std::filesystem::rename(tempPath, path, error);
    if (error)
    {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(tempPath, path, error);
    }
    if (error)
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }
    return true;
}

std::optional<std::vector<uint8_t>> ReadPreparedPrefabCacheSidecarBytes(
    const std::filesystem::path& cachePath,
    const nlohmann::json& root,
    const char* sidecarField,
    const char* hashField,
    NLS::Base::Profiling::PerformanceStageScope& scope,
    const char* byteCounterName,
    const char* stageName)
{
    NLS::Base::Profiling::PerformanceStageScope sidecarScope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        stageName,
        NLS::Base::Profiling::PerformanceStageThread::Main);
    const auto sidecar = root.find(sidecarField);
    const auto hash = root.find(hashField);
    if (sidecar == root.end() || !sidecar->is_string() ||
        hash == root.end() || !hash->is_string())
    {
        return std::nullopt;
    }

    const auto& sidecarFilename = sidecar->get_ref<const std::string&>();
    const auto& expectedHash = hash->get_ref<const std::string&>();
    if (!IsPreparedPrefabCacheSidecarFilenameSafe(sidecarFilename) || expectedHash.empty())
        return std::nullopt;

    auto bytes = ReadPreparedPrefabCacheBinaryFile(cachePath.parent_path() / sidecarFilename);
    if (!bytes.has_value())
        return std::nullopt;
    scope.AddCounter(byteCounterName, bytes->size());
    sidecarScope.AddCounter(byteCounterName, bytes->size());
    if (PreparedPrefabCacheBytesHash(*bytes) != expectedHash)
        return std::nullopt;
    return bytes;
}

void RemoveStalePreparedPrefabCacheSidecars(
    const std::filesystem::path& cachePath,
    const std::vector<std::string>& retainedFilenames)
{
    std::error_code error;
    const auto cacheDirectory = cachePath.parent_path();
    if (!std::filesystem::is_directory(cacheDirectory, error))
        return;

    const auto prefix = cachePath.stem().generic_string() + ".";
    for (const auto& entry : std::filesystem::directory_iterator(cacheDirectory, error))
    {
        if (error)
            return;
        if (!entry.is_regular_file(error) || !IsPreparedPrefabCacheSidecarExtension(entry.path()))
            continue;

        const auto filename = entry.path().filename().generic_string();
        if (filename.rfind(prefix, 0u) != 0u)
            continue;
        if (std::find(retainedFilenames.begin(), retainedFilenames.end(), filename) != retainedFilenames.end())
            continue;

        std::filesystem::remove(entry.path(), error);
        error.clear();
    }
}

std::string BuildUnifiedPrefabRuntimeCacheIdentity(const UnifiedPrefabLoadKey& key)
{
    return key.artifactIdentity +
        "|manifest@" + key.stamps.manifestStamp +
        "|dependency@" + key.stamps.dependencyStamp +
        "|prefab@" + key.stamps.prefabArtifactStamp +
        "|rendererReadiness@" + (key.rendererArtifactReadinessRequired ? "required" : "graph") +
        "|renderer@" + key.stamps.rendererArtifactStamp +
        "|importer@" + std::to_string(key.prefabImporterVersion) +
        "|reflection@" + std::to_string(key.reflectionSchemaVersion) +
        "|serialization@" + std::to_string(key.serializationFormatVersion) +
        "|dependencyManifest@" + std::to_string(key.dependencyManifestVersion);
}

PreparedPrefabCacheFreshnessRecord BuildPreparedPrefabCacheFreshnessRecord(
    const UnifiedPrefabLoadKey& key)
{
    PreparedPrefabCacheFreshnessRecord record;
    record.schemaVersion = kPersistentPreparedPrefabCacheSchemaVersion;
    record.runtimeCacheIdentity = key.runtimeCacheIdentity;
    record.manifestStamp = key.manifestStamp;
    record.dependencyStamp = key.dependencyStamp;
    record.prefabArtifactStamp = key.prefabArtifactStamp;
    record.rendererArtifactStamp = key.rendererArtifactStamp;
    record.prefabImporterVersion = key.prefabImporterVersion;
    record.reflectionSchemaVersion = key.reflectionSchemaVersion;
    record.serializationFormatVersion = key.serializationFormatVersion;
    record.dependencyManifestVersion = key.dependencyManifestVersion;
    return record;
}

std::optional<std::string> ReadPreparedPrefabCacheStringValue(
    const nlohmann::json& root,
    const char* name)
{
    const auto found = root.find(name);
    if (found == root.end() || !found->is_string())
        return std::nullopt;
    return found->get<std::string>();
}

std::optional<uint32_t> ReadPreparedPrefabCacheUIntValue(
    const nlohmann::json& root,
    const char* name)
{
    const auto found = root.find(name);
    if (found == root.end())
        return std::nullopt;
    if (found->is_number_unsigned())
    {
        const auto value = found->get<uint64_t>();
        if (value <= std::numeric_limits<uint32_t>::max())
            return static_cast<uint32_t>(value);
    }
    else if (found->is_number_integer())
    {
        const auto value = found->get<int64_t>();
        if (value >= 0 && value <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
            return static_cast<uint32_t>(value);
    }
    return std::nullopt;
}

bool PreparedPrefabCacheDigestMatches(
    const nlohmann::json& root,
    const char* name,
    const std::string& expectedText)
{
    const auto digest = ReadPreparedPrefabCacheStringValue(root, name);
    return digest.has_value() && *digest == PreparedPrefabCacheTextDigest(expectedText);
}

PreparedPrefabCacheFreshnessRecord ReadPreparedPrefabCacheFreshnessRecord(
    const nlohmann::json& root)
{
    PreparedPrefabCacheFreshnessRecord record;
    record.schemaVersion = ReadPreparedPrefabCacheUIntValue(root, "schema").value_or(0u);
    record.runtimeCacheIdentity = ReadPreparedPrefabCacheStringValue(root, "runtimeCacheIdentity").value_or(std::string {});
    record.manifestStamp = ReadPreparedPrefabCacheStringValue(root, "manifestStamp").value_or(std::string {});
    record.dependencyStamp = ReadPreparedPrefabCacheStringValue(root, "dependencyStamp").value_or(std::string {});
    record.prefabArtifactStamp = ReadPreparedPrefabCacheStringValue(root, "prefabArtifactStamp").value_or(std::string {});
    record.rendererArtifactStamp = ReadPreparedPrefabCacheStringValue(root, "rendererArtifactStamp").value_or(std::string {});
    record.prefabImporterVersion = ReadPreparedPrefabCacheUIntValue(root, "prefabImporterVersion").value_or(0u);
    record.reflectionSchemaVersion = ReadPreparedPrefabCacheUIntValue(root, "reflectionSchemaVersion").value_or(0u);
    record.serializationFormatVersion = ReadPreparedPrefabCacheUIntValue(root, "serializationFormatVersion").value_or(0u);
    record.dependencyManifestVersion = ReadPreparedPrefabCacheUIntValue(root, "dependencyManifestVersion").value_or(0u);
    return record;
}

bool IsPreparedPrefabCacheFresh(
    const PreparedPrefabCacheFreshnessRecord& record,
    const UnifiedPrefabLoadKey& key)
{
    const auto expected = BuildPreparedPrefabCacheFreshnessRecord(key);
    return record.schemaVersion == expected.schemaVersion &&
        record.runtimeCacheIdentity == expected.runtimeCacheIdentity &&
        record.manifestStamp == expected.manifestStamp &&
        record.dependencyStamp == expected.dependencyStamp &&
        record.prefabArtifactStamp == expected.prefabArtifactStamp &&
        record.rendererArtifactStamp == expected.rendererArtifactStamp &&
        record.prefabImporterVersion == expected.prefabImporterVersion &&
        record.reflectionSchemaVersion == expected.reflectionSchemaVersion &&
        record.serializationFormatVersion == expected.serializationFormatVersion &&
        record.dependencyManifestVersion == expected.dependencyManifestVersion;
}

bool IsPreparedPrefabCacheFresh(
    const nlohmann::json& root,
    const UnifiedPrefabLoadKey& key)
{
    const auto expected = BuildPreparedPrefabCacheFreshnessRecord(key);
    const auto schemaVersion = ReadPreparedPrefabCacheUIntValue(root, "schema");
    const auto prefabImporterVersion = ReadPreparedPrefabCacheUIntValue(root, "prefabImporterVersion");
    const auto reflectionSchemaVersion = ReadPreparedPrefabCacheUIntValue(root, "reflectionSchemaVersion");
    const auto serializationFormatVersion = ReadPreparedPrefabCacheUIntValue(root, "serializationFormatVersion");
    const auto dependencyManifestVersion = ReadPreparedPrefabCacheUIntValue(root, "dependencyManifestVersion");
    return schemaVersion.has_value() &&
        *schemaVersion == expected.schemaVersion &&
        PreparedPrefabCacheDigestMatches(root, "runtimeCacheIdentityDigest", expected.runtimeCacheIdentity) &&
        PreparedPrefabCacheDigestMatches(root, "manifestStampDigest", expected.manifestStamp) &&
        PreparedPrefabCacheDigestMatches(root, "dependencyStampDigest", expected.dependencyStamp) &&
        PreparedPrefabCacheDigestMatches(root, "prefabArtifactStampDigest", expected.prefabArtifactStamp) &&
        PreparedPrefabCacheDigestMatches(root, "rendererArtifactStampDigest", expected.rendererArtifactStamp) &&
        prefabImporterVersion.has_value() &&
        *prefabImporterVersion == expected.prefabImporterVersion &&
        reflectionSchemaVersion.has_value() &&
        *reflectionSchemaVersion == expected.reflectionSchemaVersion &&
        serializationFormatVersion.has_value() &&
        *serializationFormatVersion == expected.serializationFormatVersion &&
        dependencyManifestVersion.has_value() &&
        *dependencyManifestVersion == expected.dependencyManifestVersion;
}

void PrunePersistentPreparedPrefabCacheDirectory(const std::filesystem::path& cacheDirectory)
{
    struct CacheFile
    {
        std::filesystem::path path;
        std::filesystem::file_time_type lastWriteTime {};
        uintmax_t size = 0u;
        std::vector<std::filesystem::path> sidecars;
    };

    std::error_code error;
    if (!std::filesystem::is_directory(cacheDirectory, error))
        return;

    std::vector<CacheFile> files;
    std::vector<std::pair<std::filesystem::path, uintmax_t>> sidecars;
    uintmax_t retainedBytes = 0u;
    for (const auto& entry : std::filesystem::directory_iterator(cacheDirectory, error))
    {
        if (error)
            return;
        if (!entry.is_regular_file(error))
            continue;

        if (IsPreparedPrefabCacheSidecarExtension(entry.path()))
        {
            const auto sidecarSize = entry.file_size(error);
            if (!error)
                sidecars.push_back({entry.path(), sidecarSize});
            error.clear();
            continue;
        }

        if (entry.path().extension() != ".json")
            continue;

        CacheFile file;
        file.path = entry.path();
        file.lastWriteTime = entry.last_write_time(error);
        if (error)
            continue;
        file.size = entry.file_size(error);
        if (error)
            continue;

        retainedBytes += file.size;
        files.push_back(std::move(file));
    }

    std::unordered_map<std::string, size_t> cacheIndexByStem;
    cacheIndexByStem.reserve(files.size());
    for (size_t index = 0u; index < files.size(); ++index)
        cacheIndexByStem[files[index].path.stem().generic_string()] = index;

    for (const auto& [sidecarPath, sidecarSize] : sidecars)
    {
        const auto filename = sidecarPath.filename().generic_string();
        const auto separator = filename.find('.');
        if (separator == std::string::npos)
            continue;

        const auto owner = cacheIndexByStem.find(filename.substr(0u, separator));
        if (owner == cacheIndexByStem.end())
            continue;

        auto& file = files[owner->second];
        file.sidecars.push_back(sidecarPath);
        file.size += sidecarSize;
        retainedBytes += sidecarSize;
    }

    std::sort(
        files.begin(),
        files.end(),
        [](const CacheFile& lhs, const CacheFile& rhs)
        {
            return lhs.lastWriteTime < rhs.lastWriteTime;
        });

    size_t firstRetained = 0u;
    while (firstRetained < files.size() &&
        (files.size() - firstRetained > kMaxPersistentPreparedPrefabCacheEntries ||
            retainedBytes > kMaxPersistentPreparedPrefabCacheBytes))
    {
        std::filesystem::remove(files[firstRetained].path, error);
        error.clear();
        for (const auto& sidecar : files[firstRetained].sidecars)
        {
            std::filesystem::remove(sidecar, error);
            error.clear();
        }
        retainedBytes -= std::min(retainedBytes, files[firstRetained].size);
        ++firstRetained;
    }
}

size_t EstimatePrefabHotCacheBytes(const NLS::Engine::Assets::PrefabArtifact& prefab)
{
    size_t bytes = sizeof(prefab);
    bytes += prefab.graph.objects.size() * sizeof(NLS::Engine::Serialize::ObjectRecord);
    bytes += prefab.graph.overrides.size() * sizeof(NLS::Engine::Serialize::PatchOperation);
    bytes += prefab.graph.prefabInstances.size() * sizeof(NLS::Engine::Serialize::PrefabInstanceRecord);
    bytes += prefab.resolvedAssets.size() * sizeof(NLS::Engine::Assets::PrefabResolvedAsset);
    bytes += prefab.baseChain.size() * sizeof(NLS::Core::Assets::AssetId);
    bytes += prefab.sourceToRuntimeObject.size() *
        (sizeof(NLS::Engine::Serialize::ObjectId) * 2u);

    for (const auto& object : prefab.graph.objects)
    {
        bytes += object.typeName.size();
        bytes += object.debugName.size();
        bytes += object.debugPath.size();
        bytes += object.properties.size() * sizeof(NLS::Engine::Serialize::PropertyRecord);
        for (const auto& property : object.properties)
            bytes += property.name.size();
    }
    for (const auto& resolved : prefab.resolvedAssets)
    {
        bytes += resolved.expectedType.size();
        bytes += resolved.subAssetKey.size();
        bytes += resolved.artifactPath.size();
    }
    return bytes;
}

void MaybePrunePersistentPreparedPrefabCacheDirectory(const std::filesystem::path& cacheDirectory)
{
    static std::mutex pruneMutex;
    static size_t cacheWritesSincePrune = 0u;
    static std::chrono::steady_clock::time_point lastPrune = std::chrono::steady_clock::now();

    bool shouldPrune = false;
    {
        std::lock_guard<std::mutex> lock(pruneMutex);
        ++cacheWritesSincePrune;
        const auto now = std::chrono::steady_clock::now();
        if (cacheWritesSincePrune >= kPersistentPreparedPrefabCachePruneWriteInterval ||
            now - lastPrune >= std::chrono::seconds(30))
        {
            cacheWritesSincePrune = 0u;
            lastPrune = now;
            shouldPrune = true;
        }
    }

    if (shouldPrune)
        PrunePersistentPreparedPrefabCacheDirectory(cacheDirectory);
}

size_t EstimateRendererDependencyTemplateBytes(
    const std::vector<ImportedPrefabRendererDependencyTemplate>& templates)
{
    size_t bytes = templates.size() * sizeof(ImportedPrefabRendererDependencyTemplate);
    for (const auto& item : templates)
    {
        bytes += item.meshPath.size();
        bytes += item.materialPaths.size() * sizeof(std::string);
        for (const auto& materialPath : item.materialPaths)
            bytes += materialPath.size();
    }
    return bytes;
}

std::mutex& ImportedPrefabHotCacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

ImportedPrefabHotCache& ImportedPrefabHotCacheState()
{
    static ImportedPrefabHotCache cache;
    return cache;
}

std::mutex& ImportedPrefabLoadedKeyMutex()
{
    static std::mutex mutex;
    return mutex;
}

struct ImportedPrefabLoadedKeyEntry
{
    std::weak_ptr<const NLS::Engine::Assets::PrefabArtifact> prefab;
    UnifiedPrefabLoadKey key;
};

std::unordered_map<const NLS::Engine::Assets::PrefabArtifact*, ImportedPrefabLoadedKeyEntry>& ImportedPrefabLoadedKeys()
{
    static std::unordered_map<const NLS::Engine::Assets::PrefabArtifact*, ImportedPrefabLoadedKeyEntry> keys;
    return keys;
}

void PruneExpiredImportedPrefabLoadedKeys()
{
    auto& keys = ImportedPrefabLoadedKeys();
    for (auto it = keys.begin(); it != keys.end();)
    {
        if (it->second.prefab.expired())
            it = keys.erase(it);
        else
            ++it;
    }
}

void RememberImportedPrefabLoadedKey(
    const std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact>& prefab,
    const UnifiedPrefabLoadKey& key)
{
    if (!prefab)
        return;

    std::lock_guard<std::mutex> lock(ImportedPrefabLoadedKeyMutex());
    PruneExpiredImportedPrefabLoadedKeys();
    ImportedPrefabLoadedKeys()[prefab.get()] = {prefab, key};
}

std::optional<UnifiedPrefabLoadKey> FindRememberedImportedPrefabLoadedKey(
    const NLS::Engine::Assets::PrefabArtifact& prefab)
{
    std::lock_guard<std::mutex> lock(ImportedPrefabLoadedKeyMutex());
    auto& keys = ImportedPrefabLoadedKeys();
    const auto found = keys.find(&prefab);
    if (found == keys.end())
        return std::nullopt;

    if (found->second.prefab.expired())
    {
        keys.erase(found);
        return std::nullopt;
    }

    return found->second.key;
}

std::optional<FastImportedPrefabLoadResult> TryGetImportedPrefabHotCache(
    const UnifiedPrefabLoadKey& key)
{
    const auto begin = std::chrono::steady_clock::now();
    std::optional<FastImportedPrefabLoadResult> cachedResult;
    std::optional<UnifiedPrefabLoadKey> cachedKey;
    {
        std::lock_guard<std::mutex> lock(ImportedPrefabHotCacheMutex());
        auto& cache = ImportedPrefabHotCacheState();
        ++cache.useCounter;

        for (auto& entry : cache.entries)
        {
            if (!(entry.key == key))
                continue;

            entry.lastUsed = cache.useCounter;
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::CacheHit,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - begin),
                entry.estimatedBytes,
                key.source.sourceAssetPath});
            NLS::Core::Assets::CheckArtifactLoadBudget(
                NLS::Core::Assets::ArtifactLoadBudgetKind::HotCacheLookup,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - begin),
                key.source.sourceAssetPath);
            cachedResult = entry.result;
            cachedResult->key = entry.key;
            cachedKey = entry.key;
            break;
        }
    }
    if (cachedResult.has_value() && cachedKey.has_value())
    {
        RememberImportedPrefabLoadedKey(cachedResult->prefab, *cachedKey);
        return cachedResult;
    }

    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::CacheMiss,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin),
        0u,
        key.source.sourceAssetPath});
    NLS::Core::Assets::CheckArtifactLoadBudget(
        NLS::Core::Assets::ArtifactLoadBudgetKind::HotCacheLookup,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin),
        key.source.sourceAssetPath);
    return std::nullopt;
}

void EvictImportedPrefabHotCacheToBudget(ImportedPrefabHotCache& cache)
{
    while ((!cache.entries.empty() && cache.entries.size() > kMaxImportedPrefabHotCacheEntries) ||
        cache.retainedBytes > kMaxImportedPrefabHotCacheBytes)
    {
        const auto victim = std::min_element(
            cache.entries.begin(),
            cache.entries.end(),
            [](const ImportedPrefabHotCacheEntry& lhs, const ImportedPrefabHotCacheEntry& rhs)
            {
                return lhs.lastUsed < rhs.lastUsed;
            });
        if (victim == cache.entries.end())
            break;

        cache.retainedBytes -= std::min(cache.retainedBytes, victim->estimatedBytes);
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::Eviction,
            {},
            victim->estimatedBytes,
            victim->key.source.sourceAssetPath});
        cache.entries.erase(victim);
    }
}

void PutImportedPrefabHotCache(
    const UnifiedPrefabLoadKey& key,
    const FastImportedPrefabLoadResult& result)
{
    if (!result.prefab || result.rendererDependencyMissing)
        return;

    auto rendererDependencyTemplates = result.rendererDependencyTemplates;
    if (key.rendererArtifactReadinessRequired &&
        (!rendererDependencyTemplates || rendererDependencyTemplates->empty()))
    {
        rendererDependencyTemplates = std::make_shared<const std::vector<ImportedPrefabRendererDependencyTemplate>>(
            BuildImportedPrefabRendererDependencyTemplates(*result.prefab));
    }
    else if (!rendererDependencyTemplates)
    {
        rendererDependencyTemplates = std::make_shared<const std::vector<ImportedPrefabRendererDependencyTemplate>>();
    }
    const size_t estimatedBytes =
        EstimatePrefabHotCacheBytes(*result.prefab) +
        EstimateRendererDependencyTemplateBytes(*rendererDependencyTemplates) +
        result.prefabArtifactBytes;
    if (estimatedBytes > kMaxImportedPrefabHotCacheBytes)
        return;

    auto storedResult = result;
    storedResult.rendererDependencyTemplates = std::move(rendererDependencyTemplates);
    {
        std::lock_guard<std::mutex> lock(ImportedPrefabHotCacheMutex());
        auto& cache = ImportedPrefabHotCacheState();
        ++cache.useCounter;

        const auto existing = std::find_if(
            cache.entries.begin(),
            cache.entries.end(),
            [&key](const ImportedPrefabHotCacheEntry& entry)
            {
                return entry.key == key;
            });
        if (existing != cache.entries.end())
        {
            cache.retainedBytes -= std::min(cache.retainedBytes, existing->estimatedBytes);
            cache.entries.erase(existing);
        }

        cache.entries.push_back({key, std::move(storedResult), estimatedBytes, cache.useCounter});
        cache.retainedBytes += estimatedBytes;
        EvictImportedPrefabHotCacheToBudget(cache);
    }
    RememberImportedPrefabLoadedKey(result.prefab, key);
}

std::optional<FastImportedPrefabLoadResult> TryLoadPersistentPreparedPrefabCache(
    const std::filesystem::path& projectRoot,
    const UnifiedPrefabLoadKey& key,
    const bool allowSynthesizedRendererDependencyTemplates = true)
{
    NLS::Base::Profiling::PerformanceStageScope loadScope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        "LoadPreparedPrefabCache",
        NLS::Base::Profiling::PerformanceStageThread::Main);

    const auto begin = std::chrono::steady_clock::now();
    const auto cachePath = PersistentPreparedPrefabCachePath(projectRoot, key);
    auto cacheText = ReadPreparedPrefabCacheTextFile(cachePath);
    if (!cacheText.has_value())
    {
        loadScope.AddCounter("cacheMisses");
        return std::nullopt;
    }

    loadScope.AddCounter("diskByteCount", cacheText->size());
    nlohmann::json root;
    {
        NLS::Base::Profiling::PerformanceStageScope metadataParseScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "ParsePreparedPrefabCacheMetadata",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        metadataParseScope.AddCounter("diskByteCount", cacheText->size());
        root = nlohmann::json::parse(*cacheText, nullptr, false);
    }
    if (root.is_discarded() || !root.is_object())
    {
        loadScope.AddCounter("cacheMisses");
        return std::nullopt;
    }

    if (!IsPreparedPrefabCacheFresh(root, key))
    {
        loadScope.AddCounter("cacheMisses");
        return std::nullopt;
    }

    auto graphBytes = ReadPreparedPrefabCacheSidecarBytes(
        cachePath,
        root,
        "graphSidecar",
        "graphHash",
        loadScope,
        "prefabArtifactBytes",
        "ReadPreparedPrefabCacheGraphSidecar");
    if (!graphBytes.has_value())
    {
        loadScope.AddCounter("cacheMisses");
        return std::nullopt;
    }

    std::optional<NLS::Engine::Serialize::ObjectGraphDocument> document;
    {
        NLS::Base::Profiling::PerformanceStageScope graphParseScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "DecodePreparedPrefabCacheGraph",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        graphParseScope.AddCounter("prefabArtifactBytes", graphBytes->size());
        document = NLS::Engine::Serialize::ObjectGraphBinaryCache::Read(*graphBytes);
    }
    if (!document.has_value())
    {
        loadScope.AddCounter("cacheMisses");
        return std::nullopt;
    }

    auto prefab = std::make_shared<NLS::Engine::Assets::PrefabArtifact>();
    prefab->assetId = key.source.sourceAssetId;
    prefab->graph = std::move(*document);
    prefab->generatedModelPrefab = key.source.assetType == NLS::Core::Assets::AssetType::ModelScene;

    if (const auto resolvedAssets = root.find("resolvedAssets");
        resolvedAssets != root.end() && resolvedAssets->is_array())
    {
        for (const auto& resolved : *resolvedAssets)
        {
            if (!resolved.is_object())
            {
                loadScope.AddCounter("cacheMisses");
                return std::nullopt;
            }

            const auto assetIdText = resolved.value("assetId", std::string {});
            if (assetIdText.empty())
            {
                loadScope.AddCounter("cacheMisses");
                return std::nullopt;
            }
            const auto assetGuid = NLS::Guid::TryParse(assetIdText);
            if (!assetGuid.has_value())
            {
                loadScope.AddCounter("cacheMisses");
                return std::nullopt;
            }

            NLS::Engine::Assets::PrefabResolvedAsset asset;
            asset.assetId = NLS::Core::Assets::AssetId(*assetGuid);
            asset.expectedType = resolved.value("expectedType", std::string {});
            asset.subAssetKey = resolved.value("subAssetKey", std::string {});
            asset.artifactPath = resolved.value("artifactPath", std::string {});
            prefab->resolvedAssets.push_back(std::move(asset));
        }
    }

    if (const auto baseChain = root.find("baseChain");
        baseChain != root.end() && baseChain->is_array())
    {
        for (const auto& base : *baseChain)
        {
            const auto baseText = base.is_string() ? base.get<std::string>() : std::string {};
            const auto baseGuid = NLS::Guid::TryParse(baseText);
            if (baseGuid.has_value())
                prefab->baseChain.push_back(NLS::Core::Assets::AssetId(*baseGuid));
        }
    }

    const auto runtimeGraphFingerprintValue =
        NLS::Engine::Assets::BuildPrefabRuntimeResolvedGraphFingerprint(*prefab);
    const bool hasRuntimeResolvedGraphMetadata =
        root.contains("runtimeResolvedGraphSidecar") ||
        root.contains("runtimeResolvedGraphHash") ||
        root.contains("runtimeResolvedGraphFingerprint");
    if (hasRuntimeResolvedGraphMetadata)
    {
        const auto runtimeGraphFingerprintJson = root.find("runtimeResolvedGraphFingerprint");
        if (runtimeGraphFingerprintJson == root.end() ||
            !runtimeGraphFingerprintJson->is_string() ||
            runtimeGraphFingerprintJson->get_ref<const std::string&>() != std::to_string(runtimeGraphFingerprintValue))
        {
            loadScope.AddCounter("cacheMisses");
            return std::nullopt;
        }

        auto runtimeGraphBytes = ReadPreparedPrefabCacheSidecarBytes(
            cachePath,
            root,
            "runtimeResolvedGraphSidecar",
            "runtimeResolvedGraphHash",
            loadScope,
            "runtimeResolvedGraphByteCount",
            "ReadPreparedPrefabCacheRuntimeGraphSidecar");
        if (!runtimeGraphBytes.has_value())
        {
            loadScope.AddCounter("cacheMisses");
            return std::nullopt;
        }

        std::optional<NLS::Engine::Serialize::ObjectGraphDocument> runtimeDocument;
        {
            NLS::Base::Profiling::PerformanceStageScope runtimeGraphParseScope(
                NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                "DecodePreparedPrefabCacheRuntimeGraph",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            runtimeGraphParseScope.AddCounter("runtimeResolvedGraphByteCount", runtimeGraphBytes->size());
            runtimeDocument = NLS::Engine::Serialize::ObjectGraphBinaryCache::Read(*runtimeGraphBytes);
        }
        if (!runtimeDocument.has_value())
        {
            loadScope.AddCounter("cacheMisses");
            return std::nullopt;
        }

        prefab->runtimeResolvedGraph =
            std::make_shared<NLS::Engine::Serialize::ObjectGraphDocument>(std::move(*runtimeDocument));
        prefab->runtimeResolvedGraphFingerprint = runtimeGraphFingerprintValue;
        loadScope.AddCounter("runtimeResolvedGraphRestoredCount", 1u);
    }
    else if (prefab->generatedModelPrefab && !prefab->resolvedAssets.empty())
    {
        loadScope.AddCounter("cacheMisses");
        return std::nullopt;
    }

    std::vector<ImportedPrefabRendererDependencyTemplate> rendererDependencyTemplates;
    const bool rendererDependencyTemplatesRequired = key.rendererArtifactReadinessRequired;
    const bool hasRendererDependencyTemplatesField =
        root.contains("rendererDependencyTemplates");
    if (rendererDependencyTemplatesRequired)
    {
        if (const auto templates = root.find("rendererDependencyTemplates");
            templates != root.end())
        {
            if (!templates->is_array())
            {
                loadScope.AddCounter("cacheMisses");
                return std::nullopt;
            }

            for (const auto& item : *templates)
            {
                if (!item.is_object())
                {
                    loadScope.AddCounter("cacheMisses");
                    return std::nullopt;
                }

                const auto sourceObjectText = item.value("sourceObject", std::string {});
                const auto sourceObjectGuid = NLS::Guid::TryParse(sourceObjectText);
                if (!sourceObjectGuid.has_value())
                {
                    loadScope.AddCounter("cacheMisses");
                    return std::nullopt;
                }

                ImportedPrefabRendererDependencyTemplate dependency;
                dependency.sourceObject = NLS::Engine::Serialize::ObjectId(*sourceObjectGuid);
                dependency.meshPath = item.value("meshPath", std::string {});
                if (const auto materialPaths = item.find("materialPaths");
                    materialPaths != item.end() && materialPaths->is_array())
                {
                    for (const auto& materialPath : *materialPaths)
                    {
                        if (!materialPath.is_string())
                        {
                            loadScope.AddCounter("cacheMisses");
                            return std::nullopt;
                        }
                        dependency.materialPaths.push_back(materialPath.get<std::string>());
                    }
                }
                rendererDependencyTemplates.push_back(std::move(dependency));
            }
        }
    }
    else if (hasRendererDependencyTemplatesField)
    {
        loadScope.AddCounter("rendererDependencyTemplatesSkippedCount", 1u);
    }

    const auto validationFingerprint =
        NLS::Engine::Assets::BuildPrefabArtifactValidationFingerprint(*prefab);
    const auto cachedValidationFingerprint = root.find("validationFingerprint");
    if (cachedValidationFingerprint != root.end() &&
        cachedValidationFingerprint->is_string() &&
        cachedValidationFingerprint->get<std::string>() == std::to_string(validationFingerprint))
    {
        prefab->validationFingerprint = validationFingerprint;
        prefab->validationDiagnostics =
            std::make_shared<const NLS::Engine::Serialize::SerializationDiagnosticList>();
        loadScope.AddCounter("validationCachePrimedCount", 1u);
    }
    else
    {
        auto diagnostics = prefab->Validate(validationFingerprint);
        if (diagnostics.HasErrors())
        {
            loadScope.AddCounter("cacheMisses");
            return std::nullopt;
        }
    }

    FastImportedPrefabLoadResult result;
    result.prefab = std::move(prefab);
    result.key = key;
    result.prefabArtifactBytes = graphBytes->size();
    if (!hasRendererDependencyTemplatesField && rendererDependencyTemplatesRequired)
    {
        if (!allowSynthesizedRendererDependencyTemplates)
        {
            loadScope.AddCounter("cacheMisses");
            return std::nullopt;
        }
        rendererDependencyTemplates = BuildImportedPrefabRendererDependencyTemplates(*result.prefab);
    }
    result.rendererDependencyTemplates =
        std::make_shared<const std::vector<ImportedPrefabRendererDependencyTemplate>>(
            std::move(rendererDependencyTemplates));
    loadScope.AddCounter("objectCount", result.prefab->graph.objects.size());
    loadScope.AddCounter("dependencyCount", result.prefab->resolvedAssets.size());
    loadScope.AddCounter("cacheHits");
    RememberImportedPrefabLoadedKey(result.prefab, key);
    std::error_code error;
    std::filesystem::last_write_time(cachePath, std::filesystem::file_time_type::clock::now(), error);
    NLS::Core::Assets::ArtifactLoadTelemetryRecord telemetry;
    telemetry.stage = NLS::Core::Assets::ArtifactLoadTelemetryStage::CacheHit;
    telemetry.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin);
    telemetry.byteCount = graphBytes->size();
    telemetry.path = cachePath.generic_string();
    NLS::Core::Assets::RecordArtifactLoadTelemetry(telemetry);
    return result;
}

void StorePersistentPreparedPrefabCache(
    const std::filesystem::path& projectRoot,
    const UnifiedPrefabLoadKey& key,
    const FastImportedPrefabLoadResult& result)
{
    if (!result.prefab || result.rendererDependencyMissing)
        return;

    NLS::Base::Profiling::PerformanceStageScope storeScope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        "StorePreparedPrefabCache",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    storeScope.AddCounter("objectCount", result.prefab->graph.objects.size());
    storeScope.AddCounter("dependencyCount", result.prefab->resolvedAssets.size());

    const auto cachePath = PersistentPreparedPrefabCachePath(projectRoot, key);
    const auto graphBytes = NLS::Engine::Serialize::ObjectGraphBinaryCache::Write(result.prefab->graph);
    const auto graphHash = PreparedPrefabCacheBytesHash(graphBytes);
    const auto graphSidecarFilename =
        PersistentPreparedPrefabCacheSidecarFilename(cachePath, "graph", graphHash);
    storeScope.AddCounter("prefabArtifactBytes", graphBytes.size());

    nlohmann::json root = nlohmann::json::object();
    const auto freshness = BuildPreparedPrefabCacheFreshnessRecord(key);
    root["schema"] = freshness.schemaVersion;
    root["runtimeCacheIdentityDigest"] = PreparedPrefabCacheTextDigest(freshness.runtimeCacheIdentity);
    root["manifestStampDigest"] = PreparedPrefabCacheTextDigest(freshness.manifestStamp);
    root["dependencyStampDigest"] = PreparedPrefabCacheTextDigest(freshness.dependencyStamp);
    root["prefabArtifactStampDigest"] = PreparedPrefabCacheTextDigest(freshness.prefabArtifactStamp);
    root["rendererArtifactStampDigest"] = PreparedPrefabCacheTextDigest(freshness.rendererArtifactStamp);
    root["prefabImporterVersion"] = freshness.prefabImporterVersion;
    root["reflectionSchemaVersion"] = freshness.reflectionSchemaVersion;
    root["serializationFormatVersion"] = freshness.serializationFormatVersion;
    root["dependencyManifestVersion"] = freshness.dependencyManifestVersion;
    root["sourceAssetPath"] = key.source.sourceAssetPath;
    root["prefabSubAssetKey"] = key.source.prefabSubAssetKey;
    root["assetId"] = key.source.sourceAssetId.ToString();
    root["graphSidecar"] = graphSidecarFilename;
    root["graphHash"] = graphHash;
    root["resolvedAssets"] = nlohmann::json::array();
    root["baseChain"] = nlohmann::json::array();
    root["validationFingerprint"] = std::to_string(
        NLS::Engine::Assets::BuildPrefabArtifactValidationFingerprint(*result.prefab));

    for (const auto& resolved : result.prefab->resolvedAssets)
    {
        nlohmann::json item = nlohmann::json::object();
        item["assetId"] = resolved.assetId.ToString();
        item["expectedType"] = resolved.expectedType;
        item["subAssetKey"] = resolved.subAssetKey;
        item["artifactPath"] = resolved.artifactPath;
        root["resolvedAssets"].push_back(std::move(item));
    }
    for (const auto& base : result.prefab->baseChain)
        root["baseChain"].push_back(base.ToString());

    const auto runtimeGraphFingerprint =
        NLS::Engine::Assets::BuildPrefabRuntimeResolvedGraphFingerprint(*result.prefab);
    std::optional<std::vector<uint8_t>> runtimeGraphBytes;
    std::optional<std::string> runtimeGraphSidecarFilename;
    if (result.prefab->runtimeResolvedGraph &&
        result.prefab->runtimeResolvedGraphFingerprint == runtimeGraphFingerprint)
    {
        runtimeGraphBytes =
            NLS::Engine::Serialize::ObjectGraphBinaryCache::Write(*result.prefab->runtimeResolvedGraph);
        const auto runtimeGraphHash = PreparedPrefabCacheBytesHash(*runtimeGraphBytes);
        runtimeGraphSidecarFilename =
            PersistentPreparedPrefabCacheSidecarFilename(cachePath, "runtime", runtimeGraphHash);
        root["runtimeResolvedGraphSidecar"] = *runtimeGraphSidecarFilename;
        root["runtimeResolvedGraphHash"] = runtimeGraphHash;
        root["runtimeResolvedGraphFingerprint"] = std::to_string(runtimeGraphFingerprint);
        storeScope.AddCounter("runtimeResolvedGraphByteCount", runtimeGraphBytes->size());
        storeScope.AddCounter("runtimeResolvedGraphStoredCount", 1u);
    }

    if (key.rendererArtifactReadinessRequired)
    {
        root["rendererDependencyTemplates"] = nlohmann::json::array();
        auto rendererDependencyTemplates = result.rendererDependencyTemplates;
        if (!rendererDependencyTemplates || rendererDependencyTemplates->empty())
        {
            rendererDependencyTemplates = std::make_shared<const std::vector<ImportedPrefabRendererDependencyTemplate>>(
                BuildImportedPrefabRendererDependencyTemplates(*result.prefab));
        }

        for (const auto& dependency : *rendererDependencyTemplates)
        {
            nlohmann::json item = nlohmann::json::object();
            item["sourceObject"] = dependency.sourceObject.GetGuid().ToString();
            item["meshPath"] = dependency.meshPath;
            item["materialPaths"] = nlohmann::json::array();
            for (const auto& materialPath : dependency.materialPaths)
                item["materialPaths"].push_back(materialPath);
            root["rendererDependencyTemplates"].push_back(std::move(item));
        }
    }

    std::error_code error;
    std::filesystem::create_directories(cachePath.parent_path(), error);
    if (error)
        return;

    if (!WritePreparedPrefabCacheBinaryFile(cachePath.parent_path() / graphSidecarFilename, graphBytes))
        return;
    if (runtimeGraphBytes.has_value() &&
        runtimeGraphSidecarFilename.has_value() &&
        !WritePreparedPrefabCacheBinaryFile(cachePath.parent_path() / *runtimeGraphSidecarFilename, *runtimeGraphBytes))
    {
        return;
    }

    const auto cacheText = root.dump();
    storeScope.AddCounter("diskByteCount", cacheText.size());
    if (!WritePreparedPrefabCacheTextFile(cachePath, cacheText))
        return;

    std::vector<std::string> retainedSidecars = {graphSidecarFilename};
    if (runtimeGraphSidecarFilename.has_value())
        retainedSidecars.push_back(*runtimeGraphSidecarFilename);
    RemoveStalePreparedPrefabCacheSidecars(cachePath, retainedSidecars);
    MaybePrunePersistentPreparedPrefabCacheDirectory(cachePath.parent_path());
}

std::string ToEditorAssetPathFromProjectRoot(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& absolutePath)
{
    auto relative = absolutePath.lexically_normal().lexically_relative(projectRoot.lexically_normal());
    if (relative.empty() || relative.is_absolute())
        return {};

    for (const auto& part : relative)
    {
        if (part == "..")
            return {};
    }
    return NormalizeEditorAssetPath(relative);
}

bool HasDependency(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetDependencyKind kind,
    const std::string_view value,
    const std::string_view hashOrVersion)
{
    return std::any_of(
        manifest.dependencies.begin(),
        manifest.dependencies.end(),
        [kind, value, hashOrVersion](const NLS::Core::Assets::AssetDependencyRecord& dependency)
        {
            return dependency.kind == kind &&
                dependency.value == value &&
                dependency.hashOrVersion == hashOrVersion;
        });
}

bool HasCurrentExternalTextureBuildPipelineDependency(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetType assetType)
{
    if (assetType != NLS::Core::Assets::AssetType::ModelScene)
        return true;

    const bool hasTextureArtifact = std::any_of(
        manifest.subAssets.begin(),
        manifest.subAssets.end(),
        [](const NLS::Core::Assets::ImportedArtifact& artifact)
        {
            return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
        });
    if (!hasTextureArtifact)
        return true;

    return HasDependency(
        manifest,
        NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
        kExternalTextureBuildPipelineDependencyName,
        std::to_string(kExternalTexturePostprocessorVersion));
}

bool ManifestDependenciesAreCurrent(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetMeta& meta,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& absoluteAssetPath)
{
    NLS::Base::Profiling::PerformanceStageScope scope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        "ManifestDependenciesAreCurrent",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    const auto sourceHashContentReadsAtBegin = GetSourceFileHashContentReadCount();
    size_t sourceHashDependencyCount = 0u;
    size_t mappingDependencyCount = 0u;
    size_t mappingFallbackFileStampCount = 0u;
    const auto finish = [&](const bool current)
    {
        scope.AddCounter("dependencyCount", manifest.dependencies.size());
        scope.AddCounter("sourceHashDependencyCount", sourceHashDependencyCount);
        scope.AddCounter("mappingDependencyCount", mappingDependencyCount);
        scope.AddCounter("mappingFallbackFileStampCount", mappingFallbackFileStampCount);
        scope.AddCounter("current", current ? 1u : 0u);
        scope.AddCounter(
            "sourceHashContentReads",
            GetSourceFileHashContentReadCount() - sourceHashContentReadsAtBegin);
        return current;
    };

    if (manifest.importerId != meta.importerId ||
        manifest.importerVersion != meta.importerVersion ||
        manifest.targetPlatform != "editor" ||
        !HasCurrentExternalTextureBuildPipelineDependency(manifest, meta.assetType))
    {
        return finish(false);
    }

    const auto assetPath = ToEditorAssetPathFromProjectRoot(projectRoot, absoluteAssetPath);
    const auto metaPath = ToEditorAssetPathFromProjectRoot(
        projectRoot,
        NLS::Core::Assets::GetAssetMetaPath(absoluteAssetPath));
    std::vector<std::string> mappingDependencyValues;
    {
        NLS::Base::Profiling::PerformanceStageScope collectMappingScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "CollectManifestMappingDependencies",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        mappingDependencyValues.reserve(manifest.dependencies.size());
        for (const auto& dependency : manifest.dependencies)
        {
            if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping)
            {
                mappingDependencyValues.push_back(dependency.value);
                ++mappingDependencyCount;
            }
        }
        collectMappingScope.AddCounter("dependencyCount", manifest.dependencies.size());
        collectMappingScope.AddCounter("mappingDependencyCount", mappingDependencyCount);
    }
    std::vector<std::optional<std::string>> mappingFingerprints;
    {
        NLS::Base::Profiling::PerformanceStageScope mappingScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "ComputeManifestMappingFingerprints",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        mappingScope.AddCounter("mappingDependencyCount", mappingDependencyValues.size());
        mappingFingerprints = ComputeModelTextureMappingDependencyFingerprints(
            projectRoot,
            mappingDependencyValues,
            manifest.targetPlatform);
    }
    size_t mappingFingerprintIndex = 0u;

    bool checkedAsset = false;
    bool checkedMeta = false;
    {
        NLS::Base::Profiling::PerformanceStageScope checkScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "CheckManifestDependencyRecords",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        for (const auto& dependency : manifest.dependencies)
        {
            if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::SourceFileHash)
            {
                ++sourceHashDependencyCount;
                const auto normalizedValue = NormalizeEditorAssetPath(dependency.value);
                if (normalizedValue == assetPath)
                    checkedAsset = true;

                const auto dependencyPath = ResolveEditorManifestDependencyPath(projectRoot, dependency.value);
                if (!dependencyPath.has_value() ||
                    dependency.hashOrVersion != CachedSourceFileContentHash(projectRoot, *dependencyPath, false))
                {
                    FlushSourceFileHashCache(projectRoot);
                    return finish(false);
                }
                continue;
            }

            if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping)
            {
                const auto mappingFingerprint = mappingFingerprintIndex < mappingFingerprints.size()
                    ? mappingFingerprints[mappingFingerprintIndex++]
                    : std::optional<std::string> {};
                if (mappingFingerprint.has_value())
                {
                    if (*mappingFingerprint != dependency.hashOrVersion)
                        return finish(false);
                    continue;
                }

                ++mappingFallbackFileStampCount;
                const auto normalizedValue = NormalizeEditorAssetPath(dependency.value);
                if (normalizedValue == metaPath)
                    checkedMeta = true;

                const auto dependencyPath = ResolveEditorManifestDependencyPath(projectRoot, dependency.value);
                if (!dependencyPath.has_value() || dependency.hashOrVersion != FileStamp(*dependencyPath))
                    return finish(false);
                continue;
            }
        }
        checkScope.AddCounter("sourceHashDependencyCount", sourceHashDependencyCount);
        checkScope.AddCounter("mappingDependencyCount", mappingDependencyCount);
        checkScope.AddCounter("mappingFallbackFileStampCount", mappingFallbackFileStampCount);
    }

    FlushSourceFileHashCache(projectRoot);
    return finish(checkedAsset && checkedMeta);
}

bool IsPathInsideRoot(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root)
{
    const auto normalizedCandidate = candidate.lexically_normal();
    const auto normalizedRoot = root.lexically_normal();
    if (normalizedCandidate == normalizedRoot)
        return true;

    const auto relative = normalizedCandidate.lexically_relative(normalizedRoot);
    if (relative.empty() || relative.is_absolute())
        return false;

    for (const auto& part : relative)
    {
        if (part == "..")
            return false;
    }
    return true;
}

std::optional<std::filesystem::path> TryRemapImportedArtifactPathToCurrentRoot(
    const std::filesystem::path& absoluteArtifactPath,
    const std::filesystem::path& artifactRoot)
{
    if (absoluteArtifactPath.empty() || artifactRoot.empty())
        return std::nullopt;

    std::vector<std::filesystem::path> parts;
    for (const auto& part : absoluteArtifactPath.lexically_normal())
        parts.push_back(part);

    for (size_t index = 0u; index + 1u < parts.size(); ++index)
    {
        const auto artifactDirectory = parts[index].generic_string();
        if (artifactDirectory != "Artifacts" && artifactDirectory != "ArtifactStaging")
            continue;

        std::filesystem::path relative;
        for (size_t relativeIndex = index + 1u; relativeIndex < parts.size(); ++relativeIndex)
            relative /= parts[relativeIndex];
        if (relative.empty())
            return std::nullopt;

        return (artifactRoot / relative).lexically_normal();
    }

    return std::nullopt;
}

std::filesystem::path ResolveManifestArtifactPath(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot,
    const std::string& artifactPath)
{
    if (artifactPath.empty())
        return {};

    const auto path = std::filesystem::path(artifactPath);
    std::vector<std::filesystem::path> candidates;
    if (path.is_absolute())
    {
        candidates.push_back(path.lexically_normal());
        if (auto remapped = TryRemapImportedArtifactPathToCurrentRoot(path, artifactRoot);
            remapped.has_value() &&
            std::find(candidates.begin(), candidates.end(), *remapped) == candidates.end())
        {
            candidates.push_back(*remapped);
        }
    }
    else
    {
        const auto projectRelative = (projectRoot / path).lexically_normal();
        if (NLS::Core::Assets::IsContentStorageArtifactPath(path.generic_string()))
            candidates.push_back(projectRelative);
        candidates.push_back((artifactRoot / path).lexically_normal());
        if (std::find(candidates.begin(), candidates.end(), projectRelative) == candidates.end())
            candidates.push_back(projectRelative);
    }

    for (const auto& candidate : candidates)
    {
        if (!candidate.empty() &&
            IsPathInsideRoot(candidate, artifactRoot) &&
            std::filesystem::is_regular_file(candidate))
        {
            return candidate;
        }
    }

    return {};
}

struct ImportedArtifactDependencyRef
{
    std::string assetIdText;
    std::string subAssetKey;
    std::string targetPlatform;
};

std::optional<ImportedArtifactDependencyRef> ParseImportedArtifactDependencyRef(
    const NLS::Core::Assets::AssetDependencyRecord& dependency)
{
    if (dependency.kind != NLS::Core::Assets::AssetDependencyKind::ImportedArtifact)
        return std::nullopt;

    ImportedArtifactDependencyRef result;
    if (const auto separator = dependency.value.find('#');
        separator != std::string::npos)
    {
        result.assetIdText = dependency.value.substr(0u, separator);
        result.subAssetKey = dependency.value.substr(separator + 1u);
    }
    else
    {
        result.assetIdText = dependency.value;
        result.subAssetKey = dependency.hashOrVersion;
    }

    if (const auto targetSeparator = result.subAssetKey.rfind('@');
        targetSeparator != std::string::npos)
    {
        result.targetPlatform = result.subAssetKey.substr(targetSeparator + 1u);
        result.subAssetKey = result.subAssetKey.substr(0u, targetSeparator);
    }

    if (!NLS::Guid::TryParse(result.assetIdText).has_value() || result.subAssetKey.empty())
        return std::nullopt;

    return result;
}

std::string BuildRendererArtifactStamp(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot)
{
    std::vector<std::string> stamps;
    for (const auto& artifact : manifest.subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Mesh &&
            artifact.artifactType != NLS::Core::Assets::ArtifactType::Material &&
            artifact.artifactType != NLS::Core::Assets::ArtifactType::Texture)
        {
            continue;
        }

        stamps.push_back(
            artifact.subAssetKey +
            "=" +
            std::filesystem::path(artifact.artifactPath).lexically_normal().generic_string() +
            "@" +
            artifact.contentHash +
            "|file@" +
            FileStamp(ResolveManifestArtifactPath(projectRoot, artifactRoot, artifact.artifactPath)));
    }
    for (const auto& dependency : manifest.dependencies)
    {
        const auto importedRef = ParseImportedArtifactDependencyRef(dependency);
        if (!importedRef.has_value())
            continue;

        if (importedRef->subAssetKey.rfind("mesh:", 0u) != 0u &&
            importedRef->subAssetKey.rfind("material:", 0u) != 0u &&
            importedRef->subAssetKey.rfind("texture:", 0u) != 0u)
        {
            continue;
        }

        const auto dependencyTargetPlatform = importedRef->targetPlatform.empty()
            ? manifest.targetPlatform
            : importedRef->targetPlatform;
        const auto externalAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(importedRef->assetIdText));
        const auto externalArtifactRoot = projectRoot / "Library" / "Artifacts";
        const auto externalManifest = LoadArtifactManifestForSource(
            projectRoot,
            externalAssetId,
            dependencyTargetPlatform);
        if (!externalManifest.has_value())
        {
            stamps.push_back("dependencyArtifact=" + dependency.value + "@" + dependency.hashOrVersion + "|missing");
            continue;
        }

        const auto* artifact = externalManifest->FindSubAsset(importedRef->subAssetKey);
        if (artifact == nullptr)
        {
            stamps.push_back("dependencyArtifact=" + dependency.value + "@" + dependency.hashOrVersion + "|missing");
            continue;
        }

        const auto resolvedPath = ResolveManifestArtifactPath(
            projectRoot,
            externalArtifactRoot,
            artifact->artifactPath);
        stamps.push_back(
            "dependencyArtifact=" +
            dependency.value +
            "|" +
            std::filesystem::path(artifact->artifactPath).lexically_normal().generic_string() +
            "@" +
            dependency.hashOrVersion +
            "|file@" +
            FileStamp(resolvedPath));
    }

    std::sort(stamps.begin(), stamps.end());
    std::string combined;
    for (const auto& stamp : stamps)
    {
        combined += stamp;
        combined.push_back(';');
    }
    return combined;
}

std::string BuildManifestContentStamp(
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    std::vector<std::string> stamps;
    stamps.push_back("source=" + manifest.sourceAssetId.ToString());
    stamps.push_back("importer=" + manifest.importerId);
    stamps.push_back("importerVersion=" + std::to_string(manifest.importerVersion));
    stamps.push_back("target=" + manifest.targetPlatform);
    stamps.push_back("primary=" + manifest.primarySubAssetKey);
    for (const auto& dependency : manifest.dependencies)
    {
        stamps.push_back(
            "dependency=" +
            std::to_string(static_cast<int>(dependency.kind)) +
            "|" +
            NormalizeEditorAssetPath(dependency.value) +
            "@" +
            dependency.hashOrVersion);
    }
    for (const auto& artifact : manifest.subAssets)
    {
        stamps.push_back(
            "artifact=" +
            artifact.sourceAssetId.ToString() +
            "|" +
            artifact.subAssetKey +
            "|" +
            std::to_string(static_cast<int>(artifact.artifactType)) +
            "|" +
            artifact.loaderId +
            "|" +
            artifact.targetPlatform +
            "|" +
            std::filesystem::path(artifact.artifactPath).lexically_normal().generic_string() +
            "@" +
            artifact.contentHash);
    }
    std::sort(stamps.begin(), stamps.end());

    std::string combined = "artifactdb";
    for (const auto& stamp : stamps)
    {
        combined.push_back(';');
        combined += stamp;
    }
    return combined;
}

std::string BuildDependencyManifestStamp(const NLS::Core::Assets::ArtifactManifest& manifest)
{
    std::vector<std::string> stamps;
    stamps.push_back("version=" + std::to_string(kPreparedPrefabDependencyManifestVersion));
    for (const auto& dependency : manifest.dependencies)
    {
        stamps.push_back(
            std::to_string(static_cast<int>(dependency.kind)) +
            "|" +
            NormalizeEditorAssetPath(dependency.value) +
            "@" +
            dependency.hashOrVersion);
    }
    std::sort(stamps.begin(), stamps.end());

    std::string combined;
    for (const auto& stamp : stamps)
    {
        combined += stamp;
        combined.push_back(';');
    }
    return combined;
}

std::string BuildPrefabArtifactContentStamp(
    const NLS::Core::Assets::ImportedArtifact& prefabArtifact,
    const std::filesystem::path& prefabPath)
{
    return "prefab=" +
        prefabArtifact.sourceAssetId.ToString() +
        "|" +
        prefabArtifact.subAssetKey +
        "|" +
        prefabArtifact.loaderId +
        "|" +
        prefabArtifact.targetPlatform +
        "|" +
        std::filesystem::path(prefabArtifact.artifactPath).lexically_normal().generic_string() +
        "@" +
        prefabArtifact.contentHash +
        "|file@" +
        FileStamp(prefabPath);
}

std::string BuildUnifiedPrefabArtifactIdentity(const PrefabSourceIdentity& source)
{
    return source.projectRootId +
        "|" + source.sourceAssetId.ToString() +
        "|" + source.sourceAssetPath +
        "|" + source.prefabSubAssetKey +
        "|" + NLS::Core::Assets::ToString(source.assetType) +
        "|" + source.importerId +
        "|" + std::to_string(source.importerVersion);
}

UnifiedPrefabLoadKey BuildUnifiedPrefabLoadKeyFromResolvedArtifacts(
    PrefabSourceIdentity source,
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::ImportedArtifact& prefabArtifact,
    const std::filesystem::path& prefabPath,
    const bool rendererArtifactReadinessRequired,
    std::string rendererArtifactStamp)
{
    UnifiedPrefabLoadKey key;
    key.source = std::move(source);
    key.stamps.manifestStamp = BuildManifestContentStamp(manifest);
    key.stamps.dependencyStamp = BuildDependencyManifestStamp(manifest);
    key.stamps.prefabArtifactStamp = BuildPrefabArtifactContentStamp(prefabArtifact, prefabPath);
    key.stamps.rendererArtifactStamp = std::move(rendererArtifactStamp);
    key.rendererArtifactReadinessRequired = rendererArtifactReadinessRequired;
    key.manifestStamp = key.stamps.manifestStamp;
    key.dependencyStamp = key.stamps.dependencyStamp;
    key.prefabArtifactStamp = key.stamps.prefabArtifactStamp;
    key.rendererArtifactStamp = key.stamps.rendererArtifactStamp;
    key.prefabImporterVersion = key.source.importerVersion;
    key.reflectionSchemaVersion = kPreparedPrefabReflectionSchemaVersion;
    key.serializationFormatVersion = kPreparedPrefabSerializationFormatVersion;
    key.dependencyManifestVersion = kPreparedPrefabDependencyManifestVersion;
    key.artifactIdentity = BuildUnifiedPrefabArtifactIdentity(key.source);
    key.runtimeCacheIdentity = BuildUnifiedPrefabRuntimeCacheIdentity(key);
    return key;
}

std::optional<NLS::Core::Assets::ArtifactManifest> LoadFastManifest(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId sourceAssetId)
{
    NLS::Core::Assets::ArtifactLoadTelemetryRecord telemetry;
    telemetry.stage = NLS::Core::Assets::ArtifactLoadTelemetryStage::ManifestValidation;
    telemetry.path = GetProjectArtifactDatabasePath(projectRoot).generic_string();
    NLS::Core::Assets::RecordArtifactLoadTelemetry(telemetry);

    return LoadArtifactManifestFromProjectArtifactDB(projectRoot, sourceAssetId);
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path)
{
    NLS::Core::Assets::ArtifactLoadTelemetryRecord telemetry;
    telemetry.stage = NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead;
    telemetry.path = path.generic_string();
    NLS::Core::Assets::RecordArtifactLoadTelemetry(telemetry);

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};

    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

bool HasNativeArtifactHeader(const std::filesystem::path& path)
{
    std::error_code error;
    if (std::filesystem::file_size(path, error) < NLS::Core::Assets::NativeArtifactContainerHeaderSize() || error)
        return false;

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    std::vector<uint8_t> header(NLS::Core::Assets::NativeArtifactContainerHeaderSize());
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    return input.gcount() == static_cast<std::streamsize>(header.size()) &&
        NLS::Core::Assets::IsNativeArtifactContainer(header);
}

bool IsReadableMaterialArtifact(const std::filesystem::path& path)
{
    const auto bytes = ReadAllBytes(path);
    if (bytes.empty())
        return false;

    const auto container = NLS::Core::Assets::ReadNativeArtifactContainerView(
        bytes,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    return container.has_value() && container->payloadSize > 0u;
}

bool IsRendererArtifactType(const NLS::Core::Assets::ArtifactType artifactType)
{
    return artifactType == NLS::Core::Assets::ArtifactType::Mesh ||
        artifactType == NLS::Core::Assets::ArtifactType::Material ||
        artifactType == NLS::Core::Assets::ArtifactType::Texture;
}

FastImportedPrefabLoadResult MakeRendererDependencyMissingResult(
    std::string diagnosticMessage)
{
    FastImportedPrefabLoadResult result;
    result.rendererDependencyMissing = true;
    result.diagnosticCode = "dragdrop-renderer-dependency-missing";
    result.diagnosticMessage = std::move(diagnosticMessage);
    return result;
}

FastImportedPrefabLoadResult ValidateRendererArtifactReadable(
    const NLS::Core::Assets::ImportedArtifact& artifact,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot)
{
    if (!IsRendererArtifactType(artifact.artifactType))
        return {};

    const auto resolvedPath = ResolveManifestArtifactPath(projectRoot, artifactRoot, artifact.artifactPath);
    if (resolvedPath.empty())
    {
        return MakeRendererDependencyMissingResult(
            "The generated model renderer dependency is missing: " +
            artifact.subAssetKey +
            " artifactPath=" +
            artifact.artifactPath +
            " artifactRoot=" +
            artifactRoot.generic_string());
    }

    if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture &&
        !NLS::Render::Assets::LoadTextureArtifact(resolvedPath).has_value())
    {
        return MakeRendererDependencyMissingResult(
            "The generated model texture dependency is not a readable native texture artifact: " +
            artifact.subAssetKey);
    }

    if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Mesh &&
        !NLS::Render::Assets::LoadMeshArtifact(resolvedPath).has_value())
    {
        return MakeRendererDependencyMissingResult(
            "The generated model mesh dependency is not a readable native mesh artifact: " +
            artifact.subAssetKey);
    }

    if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Material &&
        !IsReadableMaterialArtifact(resolvedPath))
    {
        return MakeRendererDependencyMissingResult(
            "The generated model material dependency is not a readable native material artifact: " +
            artifact.subAssetKey);
    }

    return {};
}

bool IsRendererDependencySubAssetKey(const std::string& subAssetKey)
{
    return subAssetKey.rfind("mesh:", 0u) == 0u ||
        subAssetKey.rfind("material:", 0u) == 0u ||
        subAssetKey.rfind("texture:", 0u) == 0u;
}

FastImportedPrefabLoadResult ValidateRendererManifestDependenciesReady(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot,
    std::vector<std::string>& visitedDependencies)
{
    for (const auto& artifact : manifest.subAssets)
    {
        if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab ||
            artifact.artifactType == NLS::Core::Assets::ArtifactType::Model ||
            artifact.artifactType == NLS::Core::Assets::ArtifactType::Skeleton ||
            artifact.artifactType == NLS::Core::Assets::ArtifactType::Skin ||
            artifact.artifactType == NLS::Core::Assets::ArtifactType::AnimationClip ||
            artifact.artifactType == NLS::Core::Assets::ArtifactType::MorphTarget)
        {
            continue;
        }

        auto artifactReadiness = ValidateRendererArtifactReadable(artifact, projectRoot, artifactRoot);
        if (artifactReadiness.rendererDependencyMissing)
            return artifactReadiness;
    }

    for (const auto& dependency : manifest.dependencies)
    {
        const auto importedRef = ParseImportedArtifactDependencyRef(dependency);
        if (!importedRef.has_value())
            continue;

        if (!IsRendererDependencySubAssetKey(importedRef->subAssetKey))
            continue;

        const auto dependencyTargetPlatform = importedRef->targetPlatform.empty()
            ? manifest.targetPlatform
            : importedRef->targetPlatform;
        const auto dependencyKey =
            importedRef->assetIdText + "#" + importedRef->subAssetKey + "@" + dependencyTargetPlatform;
        if (std::find(visitedDependencies.begin(), visitedDependencies.end(), dependencyKey) !=
            visitedDependencies.end())
        {
            continue;
        }
        visitedDependencies.push_back(dependencyKey);

        const auto externalAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(importedRef->assetIdText));
        const auto externalArtifactRoot = projectRoot / "Library" / "Artifacts";
        const auto externalManifest = LoadArtifactManifestForSource(
            projectRoot,
            externalAssetId,
            dependencyTargetPlatform);
        if (!externalManifest.has_value())
        {
            return MakeRendererDependencyMissingResult(
                "The generated model renderer dependency manifest is missing: " + importedRef->subAssetKey);
        }

        const auto* artifact = externalManifest->FindSubAsset(importedRef->subAssetKey);
        if (artifact == nullptr)
        {
            return MakeRendererDependencyMissingResult(
                "The generated model renderer dependency is missing from its manifest: " + importedRef->subAssetKey);
        }

        auto artifactReadiness = ValidateRendererArtifactReadable(
            *artifact,
            projectRoot,
            externalArtifactRoot);
        if (artifactReadiness.rendererDependencyMissing)
            return artifactReadiness;

        auto nestedReadiness = ValidateRendererManifestDependenciesReady(
            *externalManifest,
            projectRoot,
            externalArtifactRoot,
            visitedDependencies);
        if (nestedReadiness.rendererDependencyMissing)
            return nestedReadiness;
    }

    return {};
}

FastImportedPrefabLoadResult GeneratedModelRendererArtifactFilesExist(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot,
    std::vector<std::string>& visitedDependencies);

FastImportedPrefabLoadResult ValidateGeneratedModelRendererArtifactsReady(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot)
{
    std::vector<std::string> visitedDependencies;
    return ValidateRendererManifestDependenciesReady(
        manifest,
        projectRoot,
        artifactRoot,
        visitedDependencies);
}

FastImportedPrefabLoadResult GeneratedModelRendererArtifactFilesExist(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot)
{
    std::vector<std::string> visitedDependencies;
    return GeneratedModelRendererArtifactFilesExist(
        manifest,
        projectRoot,
        artifactRoot,
        visitedDependencies);
}

FastImportedPrefabLoadResult GeneratedModelRendererArtifactFilesExist(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot,
    std::vector<std::string>& visitedDependencies)
{
    FastImportedPrefabLoadResult result;

    for (const auto& artifact : manifest.subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Mesh &&
            artifact.artifactType != NLS::Core::Assets::ArtifactType::Material &&
            artifact.artifactType != NLS::Core::Assets::ArtifactType::Texture)
        {
            continue;
        }

        const auto resolvedPath = ResolveManifestArtifactPath(projectRoot, artifactRoot, artifact.artifactPath);
        if (resolvedPath.empty())
        {
            result.rendererDependencyMissing = true;
            result.diagnosticCode = "dragdrop-renderer-dependency-missing";
            result.diagnosticMessage =
                "The generated model renderer dependency is missing: " +
                artifact.subAssetKey;
            return result;
        }

        if (!HasNativeArtifactHeader(resolvedPath))
        {
            result.rendererDependencyMissing = true;
            result.diagnosticCode = "dragdrop-renderer-dependency-missing";
            result.diagnosticMessage =
                "The generated model renderer dependency is not a native artifact file: " +
                artifact.subAssetKey;
            return result;
        }
    }

    for (const auto& dependency : manifest.dependencies)
    {
        const auto importedRef = ParseImportedArtifactDependencyRef(dependency);
        if (!importedRef.has_value())
            continue;

        if (!IsRendererDependencySubAssetKey(importedRef->subAssetKey))
            continue;

        const auto dependencyTargetPlatform = importedRef->targetPlatform.empty()
            ? manifest.targetPlatform
            : importedRef->targetPlatform;
        const auto dependencyKey =
            importedRef->assetIdText + "#" + importedRef->subAssetKey + "@" + dependencyTargetPlatform;
        if (std::find(visitedDependencies.begin(), visitedDependencies.end(), dependencyKey) !=
            visitedDependencies.end())
        {
            continue;
        }
        visitedDependencies.push_back(dependencyKey);

        const auto externalAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(importedRef->assetIdText));
        const auto externalArtifactRoot = projectRoot / "Library" / "Artifacts";
        const auto externalManifest = LoadArtifactManifestForSource(
            projectRoot,
            externalAssetId,
            dependencyTargetPlatform);
        if (!externalManifest.has_value())
        {
            result.rendererDependencyMissing = true;
            result.diagnosticCode = "dragdrop-renderer-dependency-missing";
            result.diagnosticMessage =
                "The generated model renderer dependency manifest is missing: " +
                importedRef->subAssetKey;
            return result;
        }

        const auto* artifact = externalManifest->FindSubAsset(importedRef->subAssetKey);
        if (artifact == nullptr)
        {
            result.rendererDependencyMissing = true;
            result.diagnosticCode = "dragdrop-renderer-dependency-missing";
            result.diagnosticMessage =
                "The generated model renderer dependency is missing from its manifest: " +
                importedRef->subAssetKey;
            return result;
        }

        const auto resolvedPath = ResolveManifestArtifactPath(
            projectRoot,
            externalArtifactRoot,
            artifact->artifactPath);
        if (resolvedPath.empty())
        {
            result.rendererDependencyMissing = true;
            result.diagnosticCode = "dragdrop-renderer-dependency-missing";
            result.diagnosticMessage =
                "The generated model renderer dependency is missing: " +
                artifact->subAssetKey +
                " artifactPath=" +
                artifact->artifactPath +
                " artifactRoot=" +
                externalArtifactRoot.generic_string();
            return result;
        }

        if (!HasNativeArtifactHeader(resolvedPath))
        {
            result.rendererDependencyMissing = true;
            result.diagnosticCode = "dragdrop-renderer-dependency-missing";
            result.diagnosticMessage =
                "The generated model renderer dependency is not a native artifact file: " +
                artifact->subAssetKey;
            return result;
        }

        auto nestedReadiness = GeneratedModelRendererArtifactFilesExist(
            *externalManifest,
            projectRoot,
            externalArtifactRoot,
            visitedDependencies);
        if (nestedReadiness.rendererDependencyMissing)
            return nestedReadiness;
    }

    return result;
}

FastImportedPrefabLoadResult LoadImportedPrefabFast(
    const std::filesystem::path& projectRoot,
    const std::string& assetPath,
    const std::string& prefabSubAssetKey,
    const NLS::Core::Assets::AssetType assetType,
    const ImportedPrefabArtifactLoadMode loadMode = ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles)
{
    FastImportedPrefabLoadResult result;
    const auto fail = [&result](std::string code, std::string message)
    {
        result.diagnosticCode = std::move(code);
        result.diagnosticMessage = std::move(message);
        return result;
    };
    const auto absolutePath = (projectRoot / std::filesystem::path(assetPath)).lexically_normal();
    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(absolutePath));
    if (!meta.has_value() || !meta->id.IsValid())
        return fail(
            "imported-prefab-meta-missing",
            "The source asset meta file is missing or has no valid asset id.");
    auto currentMeta = *meta;
    currentMeta.importerVersion = std::max(
        currentMeta.importerVersion,
        NLS::Core::Assets::GetCurrentImporterVersion(currentMeta.assetType));

    const auto artifactRoot = projectRoot / "Library" / "Artifacts";
    auto manifest = LoadFastManifest(projectRoot, currentMeta.id);
    if (!manifest.has_value() || manifest->sourceAssetId != currentMeta.id)
        return fail(
            "imported-prefab-manifest-missing",
            "The imported asset manifest is missing or belongs to another source asset.");
    if (!ManifestDependenciesAreCurrent(*manifest, currentMeta, projectRoot, absolutePath))
        return fail(
            "imported-prefab-manifest-stale",
            "The imported asset manifest dependencies are no longer current.");

    if (assetType == NLS::Core::Assets::AssetType::ModelScene &&
        loadMode == ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles)
    {
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::DependencyScan});
        auto rendererReadiness = GeneratedModelRendererArtifactFilesExist(
            *manifest,
            projectRoot,
            artifactRoot);
        if (rendererReadiness.rendererDependencyMissing)
            return rendererReadiness;
    }

    const auto* prefabArtifact = manifest->FindSubAsset(prefabSubAssetKey);
    if (!prefabArtifact ||
        prefabArtifact->artifactType != NLS::Core::Assets::ArtifactType::Prefab)
    {
        return fail(
            "imported-prefab-subasset-missing",
            "The requested prefab sub-asset is not present in the imported asset manifest.");
    }

    const auto prefabPath = ResolveManifestArtifactPath(projectRoot, artifactRoot, prefabArtifact->artifactPath);
    if (prefabPath.empty())
        return fail(
            "imported-prefab-artifact-missing",
            "The prefab artifact file referenced by the manifest is missing.");

    const auto cacheKey = BuildUnifiedPrefabLoadKeyFromResolvedArtifacts(
        NormalizePrefabSourceIdentity(
            projectRoot,
            assetPath,
            prefabSubAssetKey,
            currentMeta.id,
            assetType),
        *manifest,
        *prefabArtifact,
        prefabPath,
        IncludeRendererArtifactStamp(loadMode),
        IncludeRendererArtifactStamp(loadMode)
            ? BuildRendererArtifactStamp(*manifest, projectRoot, artifactRoot)
            : std::string {});
    result.key = cacheKey;
    if (auto cached = TryGetImportedPrefabHotCache(cacheKey); cached.has_value())
        return *cached;
    if (auto prepared = TryLoadPersistentPreparedPrefabCache(projectRoot, cacheKey); prepared.has_value())
    {
        PutImportedPrefabHotCache(cacheKey, *prepared);
        return *prepared;
    }

    const auto bytes = ReadAllBytes(prefabPath);
    if (bytes.empty())
        return fail(
            "imported-prefab-artifact-empty",
            "The prefab artifact file could not be read or is empty.");
    result.prefabArtifactBytes = bytes.size();

    const auto container = NLS::Core::Assets::ReadNativeArtifactContainerView(
        bytes,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u);
    if (!container.has_value())
        return fail(
            "imported-prefab-container-invalid",
            "The prefab artifact file is not a valid native prefab artifact container.");
    if (container->payloadSize == 0u)
        return fail(
            "imported-prefab-payload-empty",
            "The prefab artifact container has no prefab payload.");

    std::vector<NLS::Engine::Assets::PrefabResolvedAsset> resolvedAssets;
    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab)
            continue;

        auto expectedType = NLS::Engine::Assets::PrefabResolvedAssetTypeForArtifactType(artifact.artifactType);

        if (!expectedType.empty())
        {
            auto resolvedArtifactPath = ResolveManifestArtifactPath(
                projectRoot,
                artifactRoot,
                artifact.artifactPath);
            if (resolvedArtifactPath.empty())
                resolvedArtifactPath = std::filesystem::path(artifact.artifactPath).lexically_normal();

            resolvedAssets.push_back({
                artifact.sourceAssetId,
                std::move(expectedType),
                artifact.subAssetKey,
                resolvedArtifactPath.generic_string()
            });
        }
    }

    const std::string payload(
        reinterpret_cast<const char*>(container->payloadData),
        container->payloadSize);
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::PrefabGraphLoad});
    const auto validationProof = NLS::Engine::Assets::FindPrefabValidationProofFingerprint(
        container->metadata.dependencies,
        prefabSubAssetKey);
    auto graphValidationResolvedAssets =
        NLS::Engine::Assets::BuildPrefabValidationResolvedAssetsFromManifest(*manifest);
    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        payload,
        meta->id,
        std::move(resolvedAssets),
        NLS::Engine::Assets::PrefabImportOptions {
            .trustResolvedAssets = assetType == NLS::Core::Assets::AssetType::ModelScene,
            .trustGraphValidation = assetType == NLS::Core::Assets::AssetType::ModelScene,
            .trustedGraphValidationFingerprint = validationProof,
            .trustedGraphValidationResolvedAssets = std::move(graphValidationResolvedAssets)
        });
    if (importResult.diagnostics.HasErrors())
        return fail(
            "imported-prefab-import-failed",
            "The prefab artifact payload failed to import.");

    auto prefab = std::make_shared<NLS::Engine::Assets::PrefabArtifact>(
        std::move(importResult.artifact));
    prefab->generatedModelPrefab = assetType == NLS::Core::Assets::AssetType::ModelScene;
    result.prefab = std::move(prefab);
    if (cacheKey.rendererArtifactReadinessRequired)
    {
        result.rendererDependencyTemplates =
            std::make_shared<const std::vector<ImportedPrefabRendererDependencyTemplate>>(
                BuildImportedPrefabRendererDependencyTemplates(*result.prefab));
    }
    else
    {
        result.rendererDependencyTemplates =
            std::make_shared<const std::vector<ImportedPrefabRendererDependencyTemplate>>();
    }
    RememberImportedPrefabLoadedKey(result.prefab, cacheKey);
    PutImportedPrefabHotCache(cacheKey, result);
    StorePersistentPreparedPrefabCache(projectRoot, cacheKey, result);
    return result;
}

EditorAssetDragDropBridgeResult MakePendingImportedPrefabResult(
    const FastImportedPrefabLoadResult& loadResult,
    const std::string& fallbackCode,
    const std::string& fallbackMessage)
{
    EditorAssetDragDropBridgeResult result;
    result.handled = true;
    result.pendingImport = true;
    result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
    result.dragDrop.status = DragDropOperationStatus::Rejected;
    result.dragDrop.diagnostics.push_back({
        loadResult.rendererDependencyMissing ? loadResult.diagnosticCode : fallbackCode,
        loadResult.rendererDependencyMissing ? loadResult.diagnosticMessage : fallbackMessage
    });
    return result;
}

EditorAssetDragDropBridgeResult MakePendingImportedPrefabResult(
    const UnifiedPrefabLoadResult& loadResult,
    const std::string& fallbackCode,
    const std::string& fallbackMessage)
{
    EditorAssetDragDropBridgeResult result;
    result.handled = true;
    result.pendingImport = true;
    result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
    result.dragDrop.status = DragDropOperationStatus::Rejected;
    result.dragDrop.diagnostics.push_back({
        loadResult.rendererDependencyMissing && !loadResult.diagnosticCode.empty()
            ? loadResult.diagnosticCode
            : fallbackCode,
        loadResult.rendererDependencyMissing && !loadResult.diagnosticMessage.empty()
            ? loadResult.diagnosticMessage
            : fallbackMessage
    });
    return result;
}

EditorAssetDragDropBridgeResult MakePendingImportedPrefabResult(
    const UnifiedPrefabSharedLoadResult& loadResult,
    const std::string& fallbackCode,
    const std::string& fallbackMessage)
{
    EditorAssetDragDropBridgeResult result;
    result.handled = true;
    result.pendingImport = true;
    result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
    result.dragDrop.status = DragDropOperationStatus::Rejected;
    result.dragDrop.diagnostics.push_back({
        loadResult.rendererDependencyMissing && !loadResult.diagnosticCode.empty()
            ? loadResult.diagnosticCode
            : fallbackCode,
        loadResult.rendererDependencyMissing && !loadResult.diagnosticMessage.empty()
            ? loadResult.diagnosticMessage
            : fallbackMessage
    });
    return result;
}

std::optional<ImportedAssetHandle> ResolveImportedAssetHandleForPayload(
    const std::filesystem::path& projectRoot,
    const EditorAssetDragPayload& payload)
{
    auto assetPath = NormalizeProjectAssetPath(GetEditorAssetDragPayloadPath(payload));
    if (assetPath.empty())
        return std::nullopt;

    const auto payloadAssetId = GetEditorAssetDragPayloadAssetId(payload);
    if (!payloadAssetId.IsValid())
        return std::nullopt;

    const auto currentMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath((projectRoot / std::filesystem::path(assetPath)).lexically_normal()));
    if (currentMeta.has_value() &&
        currentMeta->id.IsValid() &&
        currentMeta->id != payloadAssetId)
    {
        return std::nullopt;
    }

    auto prefabSubAssetKey = GetEditorAssetDragPayloadSubAssetKey(payload);
    auto assetType = payload.generatedModelPrefab != 0u
        ? NLS::Core::Assets::AssetType::ModelScene
        : NLS::Core::Assets::InferAssetType(projectRoot / assetPath);
    prefabSubAssetKey = NormalizeGeneratedPrefabSubAssetKeyForAssetPath(
        assetPath,
        std::move(prefabSubAssetKey),
        assetType);

    if (assetType != NLS::Core::Assets::AssetType::ModelScene &&
        assetType != NLS::Core::Assets::AssetType::Prefab)
    {
        return std::nullopt;
    }

    return ImportedAssetHandle{
        std::move(assetPath),
        std::move(prefabSubAssetKey),
        assetType,
        payloadAssetId
    };
}

bool IsImportedPrefabArtifactCurrentForPayload(
    const std::filesystem::path& projectRoot,
    const EditorAssetDragPayload& payload,
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    const std::string& assetPath,
    const std::string& prefabSubAssetKey,
    const UnifiedPrefabReadiness requiredReadiness)
{
    const auto payloadAssetId = GetEditorAssetDragPayloadAssetId(payload);
    if (!payloadAssetId.IsValid() || prefab.assetId != payloadAssetId)
        return false;

    const auto absolutePath = (projectRoot / std::filesystem::path(assetPath)).lexically_normal();
    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(absolutePath));
    if (!meta.has_value() || !meta->id.IsValid() || meta->id != prefab.assetId)
        return false;

    auto currentMeta = *meta;
    currentMeta.importerVersion = std::max(
        currentMeta.importerVersion,
        NLS::Core::Assets::GetCurrentImporterVersion(currentMeta.assetType));

    const auto artifactRoot = projectRoot / "Library" / "Artifacts";
    const auto manifest = LoadFastManifest(projectRoot, currentMeta.id);
    if (!manifest.has_value() || manifest->sourceAssetId != currentMeta.id)
        return false;
    if (!ManifestDependenciesAreCurrent(*manifest, currentMeta, projectRoot, absolutePath))
        return false;

    if (requiredReadiness == UnifiedPrefabReadiness::MeshMaterialTextureReady &&
        currentMeta.assetType == NLS::Core::Assets::AssetType::ModelScene)
    {
        auto rendererReadiness = ValidateGeneratedModelRendererArtifactsReady(
            *manifest,
            projectRoot,
            artifactRoot);
        if (rendererReadiness.rendererDependencyMissing)
            return false;
    }

    const auto* prefabManifestRecord = manifest->FindSubAsset(prefabSubAssetKey);
    return prefabManifestRecord != nullptr &&
        prefabManifestRecord->artifactType == NLS::Core::Assets::ArtifactType::Prefab;
}

UnifiedPrefabLoadRequest MakeUnifiedPrefabLoadRequestForAsset(
    const std::filesystem::path& projectRoot,
    const std::string& assetPath,
    const std::string& prefabSubAssetKey,
    NLS::Core::Assets::AssetId assetId,
    const NLS::Core::Assets::AssetType assetType,
    const UnifiedPrefabLoadMode loadMode,
    const UnifiedPrefabOwnerKind ownerKind,
    std::string ownerScopeId)
{
    UnifiedPrefabLoadRequest request;
    request.source = NormalizePrefabSourceIdentity(
        projectRoot,
        assetPath,
        prefabSubAssetKey,
        assetId,
        assetType);
    request.loadMode = loadMode;
    request.ownerKind = ownerKind;
    request.ownerScopeId = std::move(ownerScopeId);
    request.requiredReadiness = UnifiedPrefabReadiness::MeshMaterialTextureReady;
    request.allowPending = true;
    return request;
}

}

std::string NormalizeGeneratedPrefabSubAssetKeyForAssetPath(
    const std::string& assetPath,
    std::string subAssetKey,
    const NLS::Core::Assets::AssetType assetType)
{
    if (assetType != NLS::Core::Assets::AssetType::ModelScene)
        return subAssetKey;

    if (subAssetKey.empty() || subAssetKey.rfind("model:", 0u) == 0u)
        return "prefab:" + std::filesystem::path(assetPath).stem().generic_string();

    return subAssetKey;
}

EditorAssetDragDropBridge::EditorAssetDragDropBridge(std::filesystem::path projectAssetsPath)
    : m_projectAssetsPath(std::move(projectAssetsPath))
{
}

std::shared_ptr<const std::vector<ImportedPrefabRendererDependencyTemplate>>
TryGetImportedPrefabRendererDependencyTemplates(const UnifiedPrefabLoadKey& key)
{
    std::lock_guard<std::mutex> lock(ImportedPrefabHotCacheMutex());
    auto& cache = ImportedPrefabHotCacheState();
    ++cache.useCounter;
    for (auto& entry : cache.entries)
    {
        if (!(entry.key == key))
            continue;
        entry.lastUsed = cache.useCounter;
        return entry.result.rendererDependencyTemplates;
    }
    return nullptr;
}

std::optional<UnifiedPrefabLoadKey> TryGetImportedPrefabLoadKeyForArtifact(
    const NLS::Engine::Assets::PrefabArtifact& prefab)
{
    return FindRememberedImportedPrefabLoadedKey(prefab);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void ClearModelTextureMappingDependencyFingerprintCacheForTesting()
{
    std::lock_guard lock(ModelTextureMappingDependencyFingerprintCacheMutex());
    ModelTextureMappingDependencyFingerprintCache().clear();
    ModelTextureMappingArtifactDatabaseTextureIndexCache().clear();
    ModelTextureMappingDependencyFingerprintCacheUseCounter() = 0u;
    ModelTextureMappingDependencyFingerprintScanCountForTestingStorage() = 0u;
    ModelTextureMappingDependencyArtifactDatabaseLoadCountForTestingStorage() = 0u;
    ModelTextureMappingDependencyMetaLoadCountForTestingStorage() = 0u;
    ModelTextureMappingDependencySourcePathFallbackCountForTestingStorage() = 0u;
}

size_t GetModelTextureMappingDependencyFingerprintScanCountForTesting()
{
    return ModelTextureMappingDependencyFingerprintScanCountForTestingStorage();
}

size_t GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting()
{
    return ModelTextureMappingDependencyArtifactDatabaseLoadCountForTestingStorage();
}

size_t GetModelTextureMappingDependencyMetaLoadCountForTesting()
{
    return ModelTextureMappingDependencyMetaLoadCountForTestingStorage();
}

size_t GetModelTextureMappingDependencySourcePathFallbackCountForTesting()
{
    return ModelTextureMappingDependencySourcePathFallbackCountForTestingStorage();
}

std::optional<std::string> ComputeModelTextureMappingDependencyFingerprintForTesting(
    const std::filesystem::path& projectRoot,
    const std::string& dependencyValue,
    const std::string& targetPlatform)
{
    return ComputeModelTextureMappingDependencyFingerprint(projectRoot, dependencyValue, targetPlatform);
}

std::vector<std::optional<std::string>> ComputeModelTextureMappingDependencyFingerprintsForTesting(
    const std::filesystem::path& projectRoot,
    const std::vector<std::string>& dependencyValues,
    const std::string& targetPlatform)
{
    return ComputeModelTextureMappingDependencyFingerprints(projectRoot, dependencyValues, targetPlatform);
}

void ClearImportedPrefabHotCacheForTesting()
{
    {
        std::lock_guard<std::mutex> lock(ImportedPrefabHotCacheMutex());
        auto& cache = ImportedPrefabHotCacheState();
        cache.entries.clear();
        cache.retainedBytes = 0u;
        cache.useCounter = 0u;
    }
    {
        std::lock_guard<std::mutex> lock(ImportedPrefabLoadedKeyMutex());
        ImportedPrefabLoadedKeys().clear();
    }
}

size_t GetImportedPrefabHotCacheEntryCountForTesting()
{
    std::lock_guard<std::mutex> lock(ImportedPrefabHotCacheMutex());
    return ImportedPrefabHotCacheState().entries.size();
}

bool ManifestDependenciesAreCurrentForTesting(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetMeta& meta,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& absoluteAssetPath)
{
    return ManifestDependenciesAreCurrent(manifest, meta, projectRoot, absoluteAssetPath);
}

PreparedPrefabCacheFreshnessRecord BuildPreparedPrefabCacheFreshnessRecordForTesting(
    const UnifiedPrefabLoadKey& key)
{
    return BuildPreparedPrefabCacheFreshnessRecord(key);
}

bool IsPreparedPrefabCacheFreshForTesting(
    const PreparedPrefabCacheFreshnessRecord& record,
    const UnifiedPrefabLoadKey& key)
{
    return IsPreparedPrefabCacheFresh(record, key);
}
#endif

std::filesystem::path EditorAssetDragDropBridge::ProjectRoot() const
{
    auto assetsPath = m_projectAssetsPath.lexically_normal();
    while (!assetsPath.empty() && !assetsPath.has_filename())
        assetsPath = assetsPath.parent_path();
    return assetsPath.parent_path();
}

std::optional<UnifiedPrefabLoadKey> EditorAssetDragDropBridge::BuildUnifiedPrefabLoadKey(
    const UnifiedPrefabLoadRequest& request) const
{
    const auto projectRoot = ProjectRoot();
    auto source = NormalizePrefabSourceIdentity(
        projectRoot,
        request.source.sourceAssetPath,
        request.source.prefabSubAssetKey,
        request.source.sourceAssetId,
        request.source.assetType);
    if (source.sourceAssetPath.empty() ||
        source.prefabSubAssetKey.empty() ||
        !source.sourceAssetId.IsValid())
    {
        return std::nullopt;
    }

    const auto absolutePath = (projectRoot / std::filesystem::path(source.sourceAssetPath)).lexically_normal();
    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(absolutePath));
    if (!meta.has_value() || !meta->id.IsValid() || meta->id != source.sourceAssetId)
        return std::nullopt;

    auto currentMeta = *meta;
    currentMeta.importerVersion = std::max(
        currentMeta.importerVersion,
        NLS::Core::Assets::GetCurrentImporterVersion(currentMeta.assetType));
    source = NormalizePrefabSourceIdentity(
        projectRoot,
        source.sourceAssetPath,
        source.prefabSubAssetKey,
        currentMeta.id,
        currentMeta.assetType);

    const auto artifactRoot = projectRoot / "Library" / "Artifacts";
    auto manifest = LoadFastManifest(projectRoot, currentMeta.id);
    if (!manifest.has_value() || manifest->sourceAssetId != currentMeta.id)
        return std::nullopt;
    if (!ManifestDependenciesAreCurrent(*manifest, currentMeta, projectRoot, absolutePath))
        return std::nullopt;

    const auto* prefabArtifact = manifest->FindSubAsset(source.prefabSubAssetKey);
    if (!prefabArtifact ||
        prefabArtifact->artifactType != NLS::Core::Assets::ArtifactType::Prefab)
    {
        return std::nullopt;
    }

    const auto prefabPath = ResolveManifestArtifactPath(projectRoot, artifactRoot, prefabArtifact->artifactPath);
    if (prefabPath.empty())
        return std::nullopt;

    auto loadMode = ImportedPrefabArtifactLoadMode::PreviewGraphOnly;
    if (request.requiredReadiness == UnifiedPrefabReadiness::MeshMaterialTextureReady)
        loadMode = ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles;

    return BuildUnifiedPrefabLoadKeyFromResolvedArtifacts(
        std::move(source),
        *manifest,
        *prefabArtifact,
        prefabPath,
        IncludeRendererArtifactStamp(loadMode),
        IncludeRendererArtifactStamp(loadMode)
            ? BuildRendererArtifactStamp(*manifest, projectRoot, artifactRoot)
            : std::string {});
}

std::optional<UnifiedPrefabLoadKey> EditorAssetDragDropBridge::TryFindImportedPrefabHotCacheKey(
    const std::string& assetPath,
    const std::string& prefabSubAssetKey,
    const NLS::Core::Assets::AssetId assetId,
    const NLS::Core::Assets::AssetType assetType,
    const UnifiedPrefabReadiness requiredReadiness) const
{
    auto source = NormalizePrefabSourceIdentity(
        ProjectRoot(),
        assetPath,
        prefabSubAssetKey,
        assetId,
        assetType);
    if (source.sourceAssetPath.empty() ||
        source.prefabSubAssetKey.empty() ||
        !source.sourceAssetId.IsValid() ||
        source.assetType == NLS::Core::Assets::AssetType::Unknown)
    {
        return std::nullopt;
    }

    const bool rendererReadinessRequired =
        requiredReadiness == UnifiedPrefabReadiness::MeshMaterialTextureReady;
    std::lock_guard<std::mutex> lock(ImportedPrefabHotCacheMutex());
    auto& cache = ImportedPrefabHotCacheState();
    ++cache.useCounter;
    for (auto it = cache.entries.rbegin(); it != cache.entries.rend(); ++it)
    {
        auto& entry = *it;
        if (entry.key.source.sourceAssetPath != source.sourceAssetPath ||
            entry.key.source.prefabSubAssetKey != source.prefabSubAssetKey ||
            entry.key.source.sourceAssetId != source.sourceAssetId ||
            entry.key.source.assetType != source.assetType ||
            !entry.result.prefab ||
            entry.result.prefab->assetId != source.sourceAssetId ||
            entry.key.rendererArtifactReadinessRequired != rendererReadinessRequired ||
            entry.key.runtimeCacheIdentity.empty())
        {
            continue;
        }

        entry.lastUsed = cache.useCounter;
        return entry.key;
    }

    return std::nullopt;
}

bool EditorAssetDragDropBridge::IsImportedPrefabHotCacheKeyCurrent(
    const std::string& assetPath,
    const std::string& prefabSubAssetKey,
    const NLS::Core::Assets::AssetId assetId,
    const NLS::Core::Assets::AssetType assetType,
    const UnifiedPrefabLoadKey& hotCacheKey) const
{
    if (assetPath.empty() ||
        prefabSubAssetKey.empty() ||
        !assetId.IsValid() ||
        hotCacheKey.runtimeCacheIdentity.empty())
    {
        return false;
    }

    auto request = MakeUnifiedPrefabLoadRequestForAsset(
        ProjectRoot(),
        assetPath,
        prefabSubAssetKey,
        assetId,
        assetType,
        UnifiedPrefabLoadMode::FinalDrop,
        UnifiedPrefabOwnerKind::SceneInstance,
        assetPath);
    request.requiredReadiness = UnifiedPrefabReadiness::MeshMaterialTextureReady;
    request.allowPending = false;

    const auto currentKey = BuildUnifiedPrefabLoadKey(request);
    return currentKey.has_value() &&
        currentKey->runtimeCacheIdentity == hotCacheKey.runtimeCacheIdentity;
}

bool EditorAssetDragDropBridge::PreloadPreparedPrefabHotCache(
    const UnifiedPrefabLoadRequest& request) const
{
    if (TryPreloadExistingPreparedPrefabHotCache(request))
        return true;

    const auto loaded = LoadUnifiedPrefabShared(request);
    return loaded.prefab != nullptr;
}

bool EditorAssetDragDropBridge::TryPreloadExistingPreparedPrefabHotCache(
    const UnifiedPrefabLoadRequest& request) const
{
    const auto key = BuildUnifiedPrefabLoadKey(request);
    if (!key.has_value())
        return false;

    if (TryGetImportedPrefabHotCache(*key).has_value())
        return true;

    auto prepared = TryLoadPersistentPreparedPrefabCache(
        ProjectRoot(),
        *key,
        false);
    if (!prepared.has_value() || !prepared->prefab)
        return false;

    PutImportedPrefabHotCache(*key, *prepared);
    return true;
}

UnifiedPrefabLoadResult EditorAssetDragDropBridge::LoadUnifiedPrefab(
    const UnifiedPrefabLoadRequest& request) const
{
    const auto shared = LoadUnifiedPrefabShared(request);
    UnifiedPrefabLoadResult result;
    result.key = shared.key;
    if (shared.prefab)
        result.prefab = *shared.prefab;
    result.pending = shared.pending;
    result.rendererDependencyMissing = shared.rendererDependencyMissing;
    result.diagnosticCode = shared.diagnosticCode;
    result.diagnosticMessage = shared.diagnosticMessage;
    return result;
}

UnifiedPrefabSharedLoadResult EditorAssetDragDropBridge::LoadUnifiedPrefabShared(
    const UnifiedPrefabLoadRequest& request) const
{
    const auto begin = std::chrono::steady_clock::now();
    UnifiedPrefabSharedLoadResult result;

    const auto projectRoot = ProjectRoot();
    auto source = NormalizePrefabSourceIdentity(
        projectRoot,
        request.source.sourceAssetPath,
        request.source.prefabSubAssetKey,
        request.source.sourceAssetId,
        request.source.assetType);
    if (source.sourceAssetPath.empty() ||
        source.prefabSubAssetKey.empty() ||
        !source.sourceAssetId.IsValid())
    {
        result.pending = request.allowPending;
        result.diagnosticCode = "prefab-load-invalid-source";
        result.diagnosticMessage = "The prefab load request has no valid source asset identity.";
        return result;
    }

    auto loadMode = ImportedPrefabArtifactLoadMode::PreviewGraphOnly;
    if (request.requiredReadiness == UnifiedPrefabReadiness::MeshMaterialTextureReady)
        loadMode = ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles;

    const auto fastLoad = LoadImportedPrefabFast(
        projectRoot,
        source.sourceAssetPath,
        source.prefabSubAssetKey,
        source.assetType,
        loadMode);
    result.prefab = fastLoad.prefab;
    result.key = fastLoad.key;
    result.rendererDependencyMissing = fastLoad.rendererDependencyMissing;
    result.diagnosticCode = fastLoad.diagnosticCode;
    result.diagnosticMessage = fastLoad.diagnosticMessage;
    result.pending = !result.prefab && request.allowPending;
    if (result.pending && result.diagnosticCode.empty())
    {
        result.diagnosticCode = "prefab-load-pending";
        result.diagnosticMessage = "The requested prefab artifact is not ready for the requested load mode.";
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin);
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::PrefabUnifiedSharedLoad,
        elapsed,
        static_cast<size_t>(result.prefab != nullptr) +
            (static_cast<size_t>(result.pending) << 1u) +
            (static_cast<size_t>(result.rendererDependencyMissing) << 2u),
        source.sourceAssetPath});
    NLS::Core::Assets::CheckArtifactLoadBudget(
        NLS::Core::Assets::ArtifactLoadBudgetKind::PrefabUnifiedSharedLoad,
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
        source.sourceAssetPath);
    return result;
}

bool EditorAssetDragDropBridge::PreloadImportedAssetHandlePrefabHotCache(
    const EditorAssetDragPayload& payload) const
{
    const auto begin = std::chrono::steady_clock::now();
    const auto handle = ResolveImportedAssetHandleForPayload(ProjectRoot(), payload);
    if (!handle.has_value())
        return false;

    auto request = MakeUnifiedPrefabLoadRequestForAsset(
        ProjectRoot(),
        handle->assetPath,
        handle->prefabSubAssetKey,
        handle->assetId,
        handle->assetType,
        UnifiedPrefabLoadMode::Prewarm,
        UnifiedPrefabOwnerKind::AsyncJob,
        handle->assetPath);
    request.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
    request.allowPending = false;
    const bool graphPreloaded = PreloadPreparedPrefabHotCache(request);
    request.requiredReadiness = UnifiedPrefabReadiness::MeshMaterialTextureReady;
    const bool rendererPreloaded = PreloadPreparedPrefabHotCache(request);
    const bool preloaded = rendererPreloaded || graphPreloaded;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin);
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::PrefabVisiblePrewarmLoad,
        elapsed,
        preloaded ? 1u : 0u,
        handle->assetPath});
    NLS::Core::Assets::CheckArtifactLoadBudget(
        NLS::Core::Assets::ArtifactLoadBudgetKind::PrefabVisiblePrewarm,
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
        handle->assetPath);
    return preloaded;
}

std::string EditorAssetDragDropBridge::NormalizeResourcePath(const std::string& resourcePath) const
{
    if (resourcePath.empty() || resourcePath.front() == ':')
        return {};

    auto normalized = NormalizeEditorAssetPath(resourcePath);
    if (normalized == "Assets" || normalized.rfind("Assets/", 0u) == 0u)
        return normalized;
    return NormalizeEditorAssetPath(std::filesystem::path("Assets") / normalized);
}

UnifiedPrefabLoadRequest MakeSceneRestoreUnifiedPrefabLoadRequest(
    PrefabSourceIdentity source,
    std::string ownerScopeId)
{
    UnifiedPrefabLoadRequest request;
    request.source = std::move(source);
    request.loadMode = UnifiedPrefabLoadMode::SceneRestore;
    request.ownerKind = UnifiedPrefabOwnerKind::SceneInstance;
    request.ownerScopeId = std::move(ownerScopeId);
    request.requiredReadiness = UnifiedPrefabReadiness::MeshMaterialTextureReady;
    request.allowPending = false;
    return request;
}

std::string EditorAssetDragDropBridge::DefaultGeneratedPrefabSubAssetKey(
    const std::string& assetPath) const
{
    return DefaultGeneratedPrefabSubAssetKeyForAssetPath(assetPath);
}

std::pair<std::string, std::string> EditorAssetDragDropBridge::NormalizePrefabResourcePath(
    const std::string& resourcePath) const
{
    const auto marker = resourcePath.find("#prefab:");
    if (marker == std::string::npos)
    {
        const auto assetPath = NormalizeResourcePath(resourcePath);
        return {assetPath, assetPath.empty() ? std::string{} : DefaultGeneratedPrefabSubAssetKey(assetPath)};
    }

    const auto assetPath = NormalizeResourcePath(resourcePath.substr(0u, marker));
    auto subAssetKey = resourcePath.substr(marker + 1u);
    constexpr std::string_view prefabExtension = ".prefab";
    if (subAssetKey.size() >= prefabExtension.size() &&
        std::equal(
            prefabExtension.rbegin(),
            prefabExtension.rend(),
            subAssetKey.rbegin(),
            [](char a, char b)
            {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(a))) ==
                    static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
            }))
    {
        subAssetKey.resize(subAssetKey.size() - prefabExtension.size());
    }
    return {assetPath, subAssetKey};
}

bool EditorAssetDragDropBridge::ImportModelIfNeeded(const std::string& resourcePath) const
{
    const auto [assetPath, prefabSubAssetKey] = NormalizePrefabResourcePath(resourcePath);
    if (assetPath.empty())
        return false;

    const auto absolutePath = ProjectRoot() / assetPath;
    const auto assetType = NLS::Core::Assets::InferAssetType(absolutePath);
    if (assetType != NLS::Core::Assets::AssetType::ModelScene &&
        assetType != NLS::Core::Assets::AssetType::Prefab)
        return false;

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(ProjectRoot()));
    if (!database.Refresh())
        return false;

    return database.LoadSubAssetAtPath(assetPath, prefabSubAssetKey).has_value();
}

EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::InstantiateImportedAsset(
    NLS::Engine::Assets::PrefabArtifact& prefab,
    const std::string& prefabSubAssetKey,
    const NLS::Core::Assets::AssetType assetType,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry* prefabInstanceRegistry,
    NLS::Engine::GameObject* parent,
    ImportProgressTracker* progressTracker,
    const std::string& progressLabel) const
{
    return InstantiateImportedAsset(
        static_cast<const NLS::Engine::Assets::PrefabArtifact&>(prefab),
        prefabSubAssetKey,
        assetType,
        scene,
        sceneAssetId,
        prefabInstanceRegistry,
        parent,
        progressTracker,
        progressLabel);
}

EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::InstantiateImportedAsset(
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    const std::string& prefabSubAssetKey,
    const NLS::Core::Assets::AssetType assetType,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry* prefabInstanceRegistry,
    NLS::Engine::GameObject* parent,
    ImportProgressTracker* progressTracker,
    const std::string& progressLabel,
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> sharedPrefab) const
{
    EditorAssetDragDropBridgeResult result;

    result.handled = true;
    (void)progressTracker;
    (void)progressLabel;

    const auto payloadKind = assetType == NLS::Core::Assets::AssetType::ModelScene
        ? DragPayloadKind::GeneratedModelPrefabAsset
        : DragPayloadKind::PrefabAsset;
    const bool deferRendererResourceResolution = assetType == NLS::Core::Assets::AssetType::ModelScene;
    result.dragDrop = AssetDragDropWorkflow().Execute({
        {payloadKind, prefab.assetId, prefabSubAssetKey, nullptr, nullptr, nullptr, {}, &prefab, std::move(sharedPrefab)},
        {DropTargetKind::Hierarchy, &scene, parent, 0u, false},
        sceneAssetId,
        DragDropOperationKind::None,
        nullptr,
        prefabInstanceRegistry,
        {},
        deferRendererResourceResolution,
        true
    });
    if (const auto key = TryGetImportedPrefabLoadKeyForArtifact(prefab); key.has_value())
        result.dragDrop.rendererDependencyTemplates = TryGetImportedPrefabRendererDependencyTemplates(*key);
    return result;
}

EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::InstantiateImportedAsset(
    AssetDatabaseFacade& database,
    const std::string& assetPath,
    const std::string& prefabSubAssetKey,
    const NLS::Core::Assets::AssetType assetType,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry* prefabInstanceRegistry,
    NLS::Engine::GameObject* parent,
    ImportProgressTracker* progressTracker,
    const std::string& progressLabel) const
{
    ScopedBlockingPrefabDropProgress progress(progressTracker, {}, progressLabel.empty() ? assetPath : progressLabel);
    progress.Report(ImportPhase::SourceParse, 0.15, "Loading prefab artifact");
    auto prefab = database.LoadPrefabArtifactAtPath(assetPath, prefabSubAssetKey);
    if (!prefab.has_value())
        return {};

    progress.Report(ImportPhase::Commit, 0.90, "Instantiating prefab");
    auto result = InstantiateImportedAsset(
        *prefab,
        prefabSubAssetKey,
        assetType,
        scene,
        sceneAssetId,
        prefabInstanceRegistry,
        parent,
        progressTracker,
        progressLabel);
    progress.FinishForDragDropResult(result.dragDrop);
    return result;
}

EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropModelAssetIntoHierarchy(
    const std::string& resourcePath,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry* prefabInstanceRegistry,
    NLS::Engine::GameObject* parent,
    ImportProgressTracker* progressTracker) const
{
    EditorAssetDragDropBridgeResult result;

    const auto [assetPath, prefabSubAssetKey] = NormalizePrefabResourcePath(resourcePath);
    if (assetPath.empty())
        return result;

    const auto absolutePath = ProjectRoot() / assetPath;
    const auto assetType = NLS::Core::Assets::InferAssetType(absolutePath);
    if (assetType != NLS::Core::Assets::AssetType::ModelScene &&
        assetType != NLS::Core::Assets::AssetType::Prefab)
        return result;
    if (!std::filesystem::exists(absolutePath))
        return result;

    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(absolutePath.lexically_normal()));
    const auto assetId = meta.has_value() ? meta->id : NLS::Core::Assets::AssetId {};
    auto finalDropRequest = MakeUnifiedPrefabLoadRequestForAsset(
        ProjectRoot(),
        assetPath,
        prefabSubAssetKey,
        assetId,
        assetType,
        UnifiedPrefabLoadMode::FinalDrop,
        UnifiedPrefabOwnerKind::SceneInstance,
        assetPath);
    ScopedBlockingPrefabDropProgress progress(progressTracker, assetId, assetPath);
    progress.Report(ImportPhase::SourceParse, 0.15, "Loading prefab artifact");
    auto fastLoad = LoadUnifiedPrefabShared(finalDropRequest);
    if (fastLoad.prefab)
    {
        progress.Report(ImportPhase::Postprocess, 0.75, "Validating prefab renderer resources");
        progress.Report(ImportPhase::Commit, 0.90, "Instantiating prefab");
        auto dropResult = InstantiateImportedAsset(
            *fastLoad.prefab,
            prefabSubAssetKey,
            assetType,
            scene,
            sceneAssetId,
            prefabInstanceRegistry,
            parent,
            progressTracker,
            assetPath,
            fastLoad.prefab);
        progress.FinishForDragDropResult(dropResult.dragDrop);
        return dropResult;
    }

    return MakePendingImportedPrefabResult(
        fastLoad,
        "dragdrop-asset-import-pending",
        "The dragged asset is not imported yet; background preimport must complete before it can be instantiated.");
}

EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropModelAssetIntoHierarchyAsync(
    const std::string& resourcePath,
    NLS::Engine::SceneSystem::Scene& scene,
    EditorAssetDragDropAsyncRequest request) const
{
    EditorAssetDragDropBridgeResult result;

    const auto [assetPath, prefabSubAssetKey] = NormalizePrefabResourcePath(resourcePath);
    if (assetPath.empty())
        return result;

    const auto absolutePath = ProjectRoot() / assetPath;
    const auto assetType = NLS::Core::Assets::InferAssetType(absolutePath);
    if (assetType != NLS::Core::Assets::AssetType::ModelScene &&
        assetType != NLS::Core::Assets::AssetType::Prefab)
        return result;
    if (!std::filesystem::exists(absolutePath))
        return result;

    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(absolutePath.lexically_normal()));
    const auto assetId = meta.has_value() ? meta->id : NLS::Core::Assets::AssetId {};
    auto finalDropRequest = MakeUnifiedPrefabLoadRequestForAsset(
        ProjectRoot(),
        assetPath,
        prefabSubAssetKey,
        assetId,
        assetType,
        UnifiedPrefabLoadMode::FinalDrop,
        UnifiedPrefabOwnerKind::SceneInstance,
        assetPath);
    ScopedBlockingPrefabDropProgress progress(request.progressTracker, assetId, assetPath);
    progress.Report(ImportPhase::SourceParse, 0.15, "Loading prefab artifact");
    auto fastLoad = LoadUnifiedPrefabShared(finalDropRequest);
    if (fastLoad.prefab)
    {
        progress.Report(ImportPhase::Postprocess, 0.75, "Validating prefab renderer resources");
        progress.Report(ImportPhase::Commit, 0.90, "Instantiating prefab");
        auto dropResult = InstantiateImportedAsset(
            *fastLoad.prefab,
            prefabSubAssetKey,
            assetType,
            scene,
            request.sceneAssetId,
            request.prefabInstanceRegistry,
            request.parent,
            request.progressTracker,
            assetPath,
            fastLoad.prefab);
        progress.FinishForDragDropResult(dropResult.dragDrop);
        return dropResult;
    }
    result = MakePendingImportedPrefabResult(
        fastLoad,
        "dragdrop-asset-import-pending",
        "The dragged asset is not imported yet; background preimport must complete before it can be instantiated.");

    if (!request.scheduleBackgroundTask || !request.completion)
    {
        result.pendingImport = false;
        result.dragDrop.status = DragDropOperationStatus::Rejected;
        result.dragDrop.diagnostics.clear();
        result.dragDrop.diagnostics.push_back({
            "dragdrop-background-task-unavailable",
            "The asset import request cannot continue because no background completion pipeline was provided."
        });
        progress.FinishForDragDropResult(result.dragDrop);
        (void)scene;
        return result;
    }

    {
        const auto roots = MakeProjectEditorAssetRoots(ProjectRoot());
        auto completion = std::make_shared<std::function<void(EditorAssetDragDropBridgeResult)>>(
            std::move(request.completion));
        auto importAndComplete =
            [roots, assetPath, progressTracker = request.progressTracker, completion]() mutable
            {
                EditorAssetDragDropBridgeResult completionResult;
                completionResult.handled = true;
                completionResult.pendingImport = false;
                completionResult.importSucceeded = false;
                completionResult.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
                completionResult.dragDrop.status = DragDropOperationStatus::Rejected;
                try
                {
                    AssetImporterFacade importer(roots);
                    const bool imported = progressTracker != nullptr
                        ? importer.SaveAndReimport(assetPath, *progressTracker)
                        : importer.SaveAndReimport(assetPath);
                    for (const auto& diagnostic : importer.GetDiagnostics())
                    {
                        completionResult.dragDrop.diagnostics.push_back({
                            diagnostic.code,
                            diagnostic.message
                        });
                    }

                    completionResult.importSucceeded = imported;
                    completionResult.dragDrop.status = imported
                        ? DragDropOperationStatus::Committed
                        : DragDropOperationStatus::Rejected;
                }
                catch (const std::exception& exception)
                {
                    completionResult.dragDrop.diagnostics.push_back({
                        "dragdrop-background-import-failed",
                        std::string("The asset import request failed before completion: ") + exception.what()
                    });
                }
                catch (...)
                {
                    completionResult.dragDrop.diagnostics.push_back({
                        "dragdrop-background-import-failed",
                        "The asset import request failed before completion with an unknown exception."
                    });
                }
                if (completion && *completion)
                    (*completion)(std::move(completionResult));
            };
        const bool scheduled = request.scheduleBackgroundTask(importAndComplete);
        if (!scheduled)
        {
            result.pendingImport = false;
            result.importSucceeded = false;
            result.dragDrop.status = DragDropOperationStatus::Rejected;
            result.dragDrop.diagnostics.clear();
            result.dragDrop.diagnostics.push_back({
                "dragdrop-background-task-rejected",
                "The asset import request could not be scheduled because the editor background task system is not accepting required work."
            });
            if (completion && *completion)
                (*completion)(result);
            progress.FinishForDragDropResult(result.dragDrop);
            (void)scene;
            return result;
        }
    }

    progress.FinishForPendingHandoff();
    (void)scene;
    return result;
}

EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropImportedAssetHandleIntoHierarchy(
    const EditorAssetDragPayload& payload,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry* prefabInstanceRegistry,
    NLS::Engine::GameObject* parent,
    ImportProgressTracker* progressTracker) const
{
    EditorAssetDragDropBridgeResult result;

    auto assetPath = NormalizeResourcePath(GetEditorAssetDragPayloadPath(payload));
    if (assetPath.empty())
        return result;

    const auto payloadAssetId = GetEditorAssetDragPayloadAssetId(payload);
    if (!payloadAssetId.IsValid())
        return result;

    const auto currentMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath((ProjectRoot() / std::filesystem::path(assetPath)).lexically_normal()));
    if (currentMeta.has_value() &&
        currentMeta->id.IsValid() &&
        currentMeta->id != payloadAssetId)
    {
        result.handled = true;
        result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
        result.dragDrop.diagnostics.push_back({
            "dragdrop-asset-identity-mismatch",
            "The dragged asset handle no longer matches the asset identity for this path."
        });
        return result;
    }

    auto prefabSubAssetKey = GetEditorAssetDragPayloadSubAssetKey(payload);
    auto assetType = payload.generatedModelPrefab != 0u
        ? NLS::Core::Assets::AssetType::ModelScene
        : NLS::Core::Assets::InferAssetType(ProjectRoot() / assetPath);
    prefabSubAssetKey = NormalizeGeneratedPrefabSubAssetKeyForAssetPath(
        assetPath,
        std::move(prefabSubAssetKey),
        assetType);

    if (assetType != NLS::Core::Assets::AssetType::ModelScene &&
        assetType != NLS::Core::Assets::AssetType::Prefab)
    {
        return result;
    }

    auto finalDropRequest = MakeUnifiedPrefabLoadRequestForAsset(
        ProjectRoot(),
        assetPath,
        prefabSubAssetKey,
        payloadAssetId,
        assetType,
        UnifiedPrefabLoadMode::FinalDrop,
        UnifiedPrefabOwnerKind::SceneInstance,
        assetPath);
    ScopedBlockingPrefabDropProgress progress(progressTracker, payloadAssetId, assetPath);
    progress.Report(ImportPhase::SourceParse, 0.15, "Loading prefab artifact");
    auto fastLoad = LoadUnifiedPrefabShared(finalDropRequest);
    if (!fastLoad.prefab)
    {
        auto pendingResult = MakePendingImportedPrefabResult(
            fastLoad,
            payload.imported == 0u ? "dragdrop-asset-import-pending" : "dragdrop-asset-artifact-missing",
            payload.imported == 0u
                ? "The dragged asset is not imported yet; import must complete before it can be instantiated."
                : "The dragged asset has no committed prefab artifact available for non-blocking instantiation.");
        if (fastLoad.rendererDependencyMissing)
            pendingResult.pendingImport = false;
        if (pendingResult.pendingImport)
            progress.FinishForPendingHandoff();
        else
            progress.FinishForDragDropResult(pendingResult.dragDrop);
        return pendingResult;
    }

    if (fastLoad.prefab->assetId != payloadAssetId)
    {
        result.handled = true;
        result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
        result.dragDrop.diagnostics.push_back({
            "dragdrop-asset-identity-mismatch",
            "The dragged asset handle no longer matches the committed artifact for this path."
        });
        return result;
    }

    progress.Report(ImportPhase::Postprocess, 0.75, "Validating prefab renderer resources");
    progress.Report(ImportPhase::Commit, 0.90, "Instantiating prefab");
    auto dropResult = InstantiateImportedAsset(
        *fastLoad.prefab,
        prefabSubAssetKey,
        assetType,
        scene,
        sceneAssetId,
        prefabInstanceRegistry,
        parent,
        progressTracker,
        assetPath,
        fastLoad.prefab);
    progress.FinishForDragDropResult(dropResult.dragDrop);
    return dropResult;
}

EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::TryDropImportedAssetHandleFromHotCacheIntoHierarchy(
    const std::string& assetPath,
    const std::string& prefabSubAssetKey,
    NLS::Core::Assets::AssetId assetId,
    NLS::Core::Assets::AssetType assetType,
    const UnifiedPrefabLoadKey& hotCacheKey,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry* prefabInstanceRegistry,
    NLS::Engine::GameObject* parent,
    ImportProgressTracker* progressTracker,
    const bool allowGraphOnlyPreview) const
{
    EditorAssetDragDropBridgeResult result;
    if (assetPath.empty() ||
        prefabSubAssetKey.empty() ||
        !assetId.IsValid() ||
        (assetType != NLS::Core::Assets::AssetType::ModelScene &&
            assetType != NLS::Core::Assets::AssetType::Prefab))
    {
        return result;
    }

    if (hotCacheKey.runtimeCacheIdentity.empty())
        return result;
    if (!hotCacheKey.rendererArtifactReadinessRequired && !allowGraphOnlyPreview)
    {
        result.handled = true;
        result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
        result.dragDrop.diagnostics.push_back({
            "dragdrop-hot-cache-key-not-renderer-ready",
            "Graph-only prefab hot-cache keys are only valid for transient Scene View preview."
        });
        return result;
    }

    auto source = NormalizePrefabSourceIdentity(
        ProjectRoot(),
        assetPath,
        prefabSubAssetKey,
        assetId,
        assetType);
    if (source.sourceAssetPath.empty() ||
        hotCacheKey.source.sourceAssetPath != source.sourceAssetPath ||
        hotCacheKey.source.prefabSubAssetKey != source.prefabSubAssetKey ||
        hotCacheKey.source.sourceAssetId != source.sourceAssetId ||
        hotCacheKey.source.assetType != source.assetType)
    {
        result.handled = true;
        result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
        result.dragDrop.diagnostics.push_back({
            "dragdrop-hot-cache-key-mismatch",
            "The dragged asset handle no longer matches the prefab hot-cache key."
        });
        return result;
    }

    if (hotCacheKey.rendererArtifactReadinessRequired &&
        !IsImportedPrefabHotCacheKeyCurrent(
            assetPath,
            prefabSubAssetKey,
            assetId,
            assetType,
            hotCacheKey))
    {
        result.handled = true;
        result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
        result.dragDrop.diagnostics.push_back({
            "dragdrop-cached-artifact-stale",
            "The cached prefab artifact is no longer current for this asset."
        });
        return result;
    }

    const auto cached = TryGetImportedPrefabHotCache(hotCacheKey);
    if (!cached.has_value() || !cached->prefab)
        return MakePendingImportedPrefabResult(
            UnifiedPrefabSharedLoadResult {},
            "dragdrop-prefab-hot-cache-pending",
            "The dragged prefab is still loading in the background.");

    if (cached->prefab->assetId != assetId)
    {
        result.handled = true;
        result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
        result.dragDrop.diagnostics.push_back({
            "dragdrop-asset-identity-mismatch",
            "The dragged asset handle no longer matches the cached prefab artifact."
        });
        return result;
    }

    return InstantiateImportedAsset(
        *cached->prefab,
        prefabSubAssetKey,
        assetType,
        scene,
        sceneAssetId,
        prefabInstanceRegistry,
        parent,
        progressTracker,
        assetPath,
        cached->prefab);
}

EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropImportedAssetHandleIntoHierarchyBlocking(
    const EditorAssetDragPayload& payload,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry* prefabInstanceRegistry,
    NLS::Engine::GameObject* parent,
    ImportProgressTracker* progressTracker) const
{
    const auto assetPath = NormalizeResourcePath(GetEditorAssetDragPayloadPath(payload));
    if (assetPath.empty())
        return {};

    bool attemptedReimport = false;
    auto reimportAsset = [&]() -> std::optional<EditorAssetDragDropBridgeResult>
    {
        attemptedReimport = true;
        AssetImporterFacade importer(MakeProjectEditorAssetRoots(ProjectRoot()));
        const bool imported = progressTracker
            ? importer.SaveAndReimport(assetPath, *progressTracker)
            : importer.SaveAndReimport(assetPath);
        if (!imported)
        {
            EditorAssetDragDropBridgeResult result;
            result.handled = true;
            result.pendingImport = false;
            result.importSucceeded = false;
            result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
            result.dragDrop.status = DragDropOperationStatus::Rejected;
            for (const auto& diagnostic : importer.GetDiagnostics())
            {
                result.dragDrop.diagnostics.push_back({
                    diagnostic.code,
                    diagnostic.message
                });
            }
            return result;
        }
        return std::nullopt;
    };

    if (payload.imported == 0u)
    {
        if (auto failure = reimportAsset(); failure.has_value())
            return *failure;
    }

    auto result = DropImportedAssetHandleIntoHierarchy(
        payload,
        scene,
        sceneAssetId,
        prefabInstanceRegistry,
        parent,
        progressTracker);
    const auto shouldReimportAfterInitialDrop =
        !attemptedReimport &&
        !result.dragDrop.diagnostics.empty() &&
        (result.dragDrop.diagnostics.front().code == "dragdrop-renderer-dependency-missing" ||
         result.dragDrop.diagnostics.front().code == "dragdrop-asset-artifact-missing");
    if (shouldReimportAfterInitialDrop)
    {
        if (auto failure = reimportAsset(); failure.has_value())
            return *failure;

        result = DropImportedAssetHandleIntoHierarchy(
            payload,
            scene,
            sceneAssetId,
            prefabInstanceRegistry,
            parent,
            progressTracker);
    }
    return result;
}

EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropImportedPrefabArtifactIntoHierarchy(
    const EditorAssetDragPayload& payload,
    NLS::Engine::Assets::PrefabArtifact& prefab,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry* prefabInstanceRegistry,
    NLS::Engine::GameObject* parent,
    ImportProgressTracker* progressTracker) const
{
    return DropImportedPrefabArtifactIntoHierarchy(
        payload,
        static_cast<const NLS::Engine::Assets::PrefabArtifact&>(prefab),
        scene,
        sceneAssetId,
        prefabInstanceRegistry,
        parent,
        progressTracker);
}

EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropImportedPrefabArtifactIntoHierarchy(
    const EditorAssetDragPayload& payload,
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Core::Assets::AssetId sceneAssetId,
    PrefabInstanceRegistry* prefabInstanceRegistry,
    NLS::Engine::GameObject* parent,
    ImportProgressTracker* progressTracker) const
{
    EditorAssetDragDropBridgeResult result;

    auto assetPath = NormalizeResourcePath(GetEditorAssetDragPayloadPath(payload));
    if (assetPath.empty())
        return result;

    const auto payloadAssetId = GetEditorAssetDragPayloadAssetId(payload);
    if (!payloadAssetId.IsValid())
        return result;

    if (prefab.assetId != payloadAssetId)
    {
        result.handled = true;
        result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
        result.dragDrop.diagnostics.push_back({
            "dragdrop-asset-identity-mismatch",
            "The dragged asset handle no longer matches the cached prefab artifact."
        });
        return result;
    }

    auto prefabSubAssetKey = GetEditorAssetDragPayloadSubAssetKey(payload);
    const auto assetType = prefab.generatedModelPrefab
        ? NLS::Core::Assets::AssetType::ModelScene
        : NLS::Core::Assets::InferAssetType(ProjectRoot() / assetPath);
    prefabSubAssetKey = NormalizeGeneratedPrefabSubAssetKeyForAssetPath(
        assetPath,
        std::move(prefabSubAssetKey),
        assetType);

    if (assetType != NLS::Core::Assets::AssetType::ModelScene &&
        assetType != NLS::Core::Assets::AssetType::Prefab)
    {
        return result;
    }

    if (!IsImportedPrefabArtifactCurrentForPayload(
            ProjectRoot(),
            payload,
            prefab,
            assetPath,
            prefabSubAssetKey,
            UnifiedPrefabReadiness::MeshMaterialTextureReady))
    {
        result.handled = true;
        result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
        result.dragDrop.diagnostics.push_back({
            "dragdrop-cached-artifact-stale",
            "The cached prefab artifact is no longer current for this asset."
        });
        return result;
    }

    ScopedBlockingPrefabDropProgress progress(progressTracker, prefab.assetId, assetPath);
    progress.Report(ImportPhase::Postprocess, 0.75, "Validating cached prefab artifact");
    progress.Report(ImportPhase::Commit, 0.90, "Instantiating prefab");
    auto dropResult = InstantiateImportedAsset(
        prefab,
        prefabSubAssetKey,
        assetType,
        scene,
        sceneAssetId,
        prefabInstanceRegistry,
        parent,
        progressTracker,
        assetPath);
    progress.FinishForDragDropResult(dropResult.dragDrop);
    return dropResult;
}

std::optional<NLS::Engine::Assets::PrefabArtifact> EditorAssetDragDropBridge::TryLoadImportedPrefabArtifact(
    const std::string& assetPath,
    const std::string& prefabSubAssetKey) const
{
    auto prefab = TryLoadImportedPrefabArtifactShared(assetPath, prefabSubAssetKey);
    if (!prefab)
        return std::nullopt;

    return *prefab;
}

std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> EditorAssetDragDropBridge::TryLoadImportedPrefabArtifactShared(
    const std::string& assetPath,
    const std::string& prefabSubAssetKey) const
{
    const auto normalizedAssetPath = NormalizeResourcePath(assetPath);
    if (normalizedAssetPath.empty() || prefabSubAssetKey.empty())
        return {};

    const auto assetType = NLS::Core::Assets::InferAssetType(ProjectRoot() / normalizedAssetPath);
    if (assetType != NLS::Core::Assets::AssetType::ModelScene &&
        assetType != NLS::Core::Assets::AssetType::Prefab)
    {
        return {};
    }

    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath((ProjectRoot() / normalizedAssetPath).lexically_normal()));
    const auto assetId = meta.has_value() ? meta->id : NLS::Core::Assets::AssetId {};
    auto fastLoad = LoadUnifiedPrefabShared(MakeUnifiedPrefabLoadRequestForAsset(
        ProjectRoot(),
        normalizedAssetPath,
        prefabSubAssetKey,
        assetId,
        assetType,
        UnifiedPrefabLoadMode::SceneRestore,
        UnifiedPrefabOwnerKind::SceneInstance,
        normalizedAssetPath));
    if (!fastLoad.prefab)
        return {};

    return fastLoad.prefab;
}
}
