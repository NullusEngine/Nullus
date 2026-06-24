#include "Assets/EditorAssetDragDropBridge.h"

#include "Assets/AssetMeta.h"
#include "Assets/AssetImporterFacade.h"
#include "Assets/ArtifactDatabaseManifestUtils.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/ExternalAssetImporter.h"
#include "Assets/ImportedPrefabRendererDependencyTemplates.h"
#include "Assets/NativeArtifactContainer.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphWriter.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <Json/json.hpp>
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
    std::vector<ImportedPrefabRendererDependencyTemplate> rendererDependencyTemplates;
    size_t prefabArtifactBytes = 0u;
    bool rendererDependencyMissing = false;
    std::string diagnosticCode;
    std::string diagnosticMessage;
};

enum class ImportedPrefabArtifactLoadMode
{
    RequireRendererArtifactFiles,
    ValidateRendererDependencies,
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

constexpr size_t kMaxImportedPrefabHotCacheEntries = 16u;
constexpr size_t kMaxImportedPrefabHotCacheBytes = 64u * 1024u * 1024u;
constexpr uint32_t kPersistentPreparedPrefabCacheSchemaVersion = 1u;
constexpr size_t kMaxPersistentPreparedPrefabCacheEntries = 512u;
constexpr uintmax_t kMaxPersistentPreparedPrefabCacheBytes = 256u * 1024u * 1024u;
constexpr size_t kPersistentPreparedPrefabCachePruneWriteInterval = 16u;

std::string DefaultGeneratedPrefabSubAssetKeyForAssetPath(const std::string& assetPath)
{
    return "prefab:" + std::filesystem::path(assetPath).stem().generic_string();
}

std::string NormalizeGeneratedPrefabSubAssetKeyForAssetPath(
    const std::string& assetPath,
    std::string subAssetKey,
    const NLS::Core::Assets::AssetType assetType)
{
    if (assetType != NLS::Core::Assets::AssetType::ModelScene)
        return subAssetKey;

    if (subAssetKey.empty() || subAssetKey.rfind("model:", 0u) == 0u)
        return DefaultGeneratedPrefabSubAssetKeyForAssetPath(assetPath);

    return subAssetKey;
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

std::filesystem::path PersistentPreparedPrefabCachePath(
    const std::filesystem::path& projectRoot,
    const UnifiedPrefabLoadKey& key)
{
    return projectRoot /
        "Library" /
        "PreparedPrefabCache" /
        (Hex64(StableFnv1a64(key.runtimeCacheIdentity)) + ".json");
}

void PrunePersistentPreparedPrefabCacheDirectory(const std::filesystem::path& cacheDirectory)
{
    struct CacheFile
    {
        std::filesystem::path path;
        std::filesystem::file_time_type lastWriteTime {};
        uintmax_t size = 0u;
    };

    std::error_code error;
    if (!std::filesystem::is_directory(cacheDirectory, error))
        return;

    std::vector<CacheFile> files;
    uintmax_t retainedBytes = 0u;
    for (const auto& entry : std::filesystem::directory_iterator(cacheDirectory, error))
    {
        if (error)
            return;
        if (!entry.is_regular_file(error) || entry.path().extension() != ".json")
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
    if (rendererDependencyTemplates.empty())
        rendererDependencyTemplates = BuildImportedPrefabRendererDependencyTemplates(*result.prefab);
    const size_t estimatedBytes =
        EstimatePrefabHotCacheBytes(*result.prefab) +
        EstimateRendererDependencyTemplateBytes(rendererDependencyTemplates) +
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
    const auto begin = std::chrono::steady_clock::now();
    const auto cachePath = PersistentPreparedPrefabCachePath(projectRoot, key);
    std::ifstream input(cachePath, std::ios::binary);
    if (!input)
        return std::nullopt;

    const std::string cacheText((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto root = nlohmann::json::parse(cacheText, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return std::nullopt;

    if (root.value("schema", 0u) != kPersistentPreparedPrefabCacheSchemaVersion ||
        root.value("runtimeCacheIdentity", std::string {}) != key.runtimeCacheIdentity ||
        root.value("manifestStamp", std::string {}) != key.manifestStamp ||
        root.value("prefabArtifactStamp", std::string {}) != key.prefabArtifactStamp ||
        root.value("rendererArtifactStamp", std::string {}) != key.rendererArtifactStamp)
    {
        return std::nullopt;
    }

    const auto graphText = root.value("graph", std::string {});
    auto document = NLS::Engine::Serialize::ObjectGraphReader::Read(graphText);
    if (!document.has_value())
        return std::nullopt;

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
                return std::nullopt;

            const auto assetIdText = resolved.value("assetId", std::string {});
            if (assetIdText.empty())
                return std::nullopt;
            const auto assetGuid = NLS::Guid::TryParse(assetIdText);
            if (!assetGuid.has_value())
                return std::nullopt;

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

    std::vector<ImportedPrefabRendererDependencyTemplate> rendererDependencyTemplates;
    const bool hasRendererDependencyTemplatesField =
        root.contains("rendererDependencyTemplates");
    if (const auto templates = root.find("rendererDependencyTemplates");
        templates != root.end() && templates->is_array())
    {
        for (const auto& item : *templates)
        {
            if (!item.is_object())
                return std::nullopt;

            const auto sourceObjectText = item.value("sourceObject", std::string {});
            const auto sourceObjectGuid = NLS::Guid::TryParse(sourceObjectText);
            if (!sourceObjectGuid.has_value())
                return std::nullopt;

            ImportedPrefabRendererDependencyTemplate dependency;
            dependency.sourceObject = NLS::Engine::Serialize::ObjectId(*sourceObjectGuid);
            dependency.meshPath = item.value("meshPath", std::string {});
            if (const auto materialPaths = item.find("materialPaths");
                materialPaths != item.end() && materialPaths->is_array())
            {
                for (const auto& materialPath : *materialPaths)
                {
                    if (!materialPath.is_string())
                        return std::nullopt;
                    dependency.materialPaths.push_back(materialPath.get<std::string>());
                }
            }
            rendererDependencyTemplates.push_back(std::move(dependency));
        }
    }

    auto diagnostics = prefab->Validate();
    if (diagnostics.HasErrors())
        return std::nullopt;

    FastImportedPrefabLoadResult result;
    result.prefab = std::move(prefab);
    result.key = key;
    result.prefabArtifactBytes = graphText.size();
    if (!hasRendererDependencyTemplatesField)
    {
        if (!allowSynthesizedRendererDependencyTemplates)
            return std::nullopt;
        rendererDependencyTemplates = BuildImportedPrefabRendererDependencyTemplates(*result.prefab);
    }
    result.rendererDependencyTemplates = std::move(rendererDependencyTemplates);
    RememberImportedPrefabLoadedKey(result.prefab, key);
    std::error_code error;
    std::filesystem::last_write_time(cachePath, std::filesystem::file_time_type::clock::now(), error);
    NLS::Core::Assets::ArtifactLoadTelemetryRecord telemetry;
    telemetry.stage = NLS::Core::Assets::ArtifactLoadTelemetryStage::CacheHit;
    telemetry.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin);
    telemetry.byteCount = graphText.size();
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

    nlohmann::json root = nlohmann::json::object();
    root["schema"] = kPersistentPreparedPrefabCacheSchemaVersion;
    root["runtimeCacheIdentity"] = key.runtimeCacheIdentity;
    root["manifestStamp"] = key.manifestStamp;
    root["prefabArtifactStamp"] = key.prefabArtifactStamp;
    root["rendererArtifactStamp"] = key.rendererArtifactStamp;
    root["sourceAssetPath"] = key.source.sourceAssetPath;
    root["prefabSubAssetKey"] = key.source.prefabSubAssetKey;
    root["assetId"] = key.source.sourceAssetId.ToString();
    root["graph"] = NLS::Engine::Serialize::ObjectGraphWriter::Write(result.prefab->graph);
    root["resolvedAssets"] = nlohmann::json::array();
    root["baseChain"] = nlohmann::json::array();
    root["rendererDependencyTemplates"] = nlohmann::json::array();

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

    auto rendererDependencyTemplates = result.rendererDependencyTemplates;
    if (rendererDependencyTemplates.empty())
        rendererDependencyTemplates = BuildImportedPrefabRendererDependencyTemplates(*result.prefab);

    for (const auto& dependency : rendererDependencyTemplates)
    {
        nlohmann::json item = nlohmann::json::object();
        item["sourceObject"] = dependency.sourceObject.GetGuid().ToString();
        item["meshPath"] = dependency.meshPath;
        item["materialPaths"] = nlohmann::json::array();
        for (const auto& materialPath : dependency.materialPaths)
            item["materialPaths"].push_back(materialPath);
        root["rendererDependencyTemplates"].push_back(std::move(item));
    }

    const auto cachePath = PersistentPreparedPrefabCachePath(projectRoot, key);
    std::error_code error;
    std::filesystem::create_directories(cachePath.parent_path(), error);
    if (error)
        return;

    const auto tempPath = cachePath.string() + ".tmp";
    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return;

        output << root.dump();
    }

    error.clear();
    std::filesystem::rename(tempPath, cachePath, error);
    if (error)
    {
        std::filesystem::remove(cachePath, error);
        error.clear();
        std::filesystem::rename(tempPath, cachePath, error);
    }
    if (!error)
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
    if (manifest.importerId != meta.importerId ||
        manifest.importerVersion != meta.importerVersion ||
        manifest.targetPlatform != "editor" ||
        !HasCurrentExternalTextureBuildPipelineDependency(manifest, meta.assetType))
    {
        return false;
    }

    const auto assetPath = ToEditorAssetPathFromProjectRoot(projectRoot, absoluteAssetPath);
    const auto metaPath = ToEditorAssetPathFromProjectRoot(
        projectRoot,
        NLS::Core::Assets::GetAssetMetaPath(absoluteAssetPath));

    bool checkedAsset = false;
    bool checkedMeta = false;
    for (const auto& dependency : manifest.dependencies)
    {
        if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::SourceFileHash)
        {
            const auto normalizedValue = NormalizeEditorAssetPath(dependency.value);
            if (normalizedValue == assetPath)
                checkedAsset = true;

            const auto dependencyPath = ResolveEditorManifestDependencyPath(projectRoot, dependency.value);
            if (!dependencyPath.has_value() || dependency.hashOrVersion != FileStamp(*dependencyPath))
                return false;
            continue;
        }

        if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping)
        {
            const auto normalizedValue = NormalizeEditorAssetPath(dependency.value);
            if (normalizedValue == metaPath)
                checkedMeta = true;

            const auto dependencyPath = ResolveEditorManifestDependencyPath(projectRoot, dependency.value);
            if (!dependencyPath.has_value() || dependency.hashOrVersion != FileStamp(*dependencyPath))
                return false;
            continue;
        }
    }

    return checkedAsset && checkedMeta;
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
        candidates.push_back((artifactRoot / path).lexically_normal());
        const auto projectRelative = (projectRoot / path).lexically_normal();
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
    std::string rendererArtifactStamp)
{
    UnifiedPrefabLoadKey key;
    key.source = std::move(source);
    key.stamps.manifestStamp = BuildManifestContentStamp(manifest);
    key.stamps.prefabArtifactStamp = BuildPrefabArtifactContentStamp(prefabArtifact, prefabPath);
    key.stamps.rendererArtifactStamp = std::move(rendererArtifactStamp);
    key.manifestStamp = key.stamps.manifestStamp;
    key.prefabArtifactStamp = key.stamps.prefabArtifactStamp;
    key.rendererArtifactStamp = key.stamps.rendererArtifactStamp;
    key.artifactIdentity = BuildUnifiedPrefabArtifactIdentity(key.source);
    key.runtimeCacheIdentity = key.artifactIdentity +
        "|manifest@" + key.stamps.manifestStamp +
        "|prefab@" + key.stamps.prefabArtifactStamp +
        "|renderer@" + key.stamps.rendererArtifactStamp;
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

std::string ExpectedPrefabResolvedAssetType(const NLS::Core::Assets::ArtifactType type)
{
    using NLS::Core::Assets::ArtifactType;
    switch (type)
    {
    case ArtifactType::Mesh: return "Mesh";
    case ArtifactType::Material: return "Material";
    case ArtifactType::Texture: return "Texture";
    case ArtifactType::Skeleton: return "Skeleton";
    case ArtifactType::Skin: return "Skin";
    case ArtifactType::AnimationClip: return "AnimationClip";
    case ArtifactType::MorphTarget: return "MorphTarget";
    case ArtifactType::Model: return "Model";
    case ArtifactType::Shader: return "Shader";
    case ArtifactType::Scene: return "Scene";
    case ArtifactType::Audio: return "Audio";
    case ArtifactType::Prefab:
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        return {};
    }
    return {};
}

FastImportedPrefabLoadResult ValidateGeneratedModelRendererArtifactsReady(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot)
{
    FastImportedPrefabLoadResult result;

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

        if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture &&
            !NLS::Render::Assets::LoadTextureArtifact(resolvedPath).has_value())
        {
            result.rendererDependencyMissing = true;
            result.diagnosticCode = "dragdrop-renderer-dependency-missing";
            result.diagnosticMessage =
                "The generated model texture dependency is not a readable native texture artifact: " +
                artifact.subAssetKey;
            return result;
        }

        if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Mesh &&
            !NLS::Render::Assets::LoadMeshArtifact(resolvedPath).has_value())
        {
            result.rendererDependencyMissing = true;
            result.diagnosticCode = "dragdrop-renderer-dependency-missing";
            result.diagnosticMessage =
                "The generated model mesh dependency is not a readable native mesh artifact: " +
                artifact.subAssetKey;
            return result;
        }

        if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Material &&
            !IsReadableMaterialArtifact(resolvedPath))
        {
            result.rendererDependencyMissing = true;
            result.diagnosticCode = "dragdrop-renderer-dependency-missing";
            result.diagnosticMessage =
                "The generated model material dependency is not a readable native material artifact: " +
                artifact.subAssetKey;
            return result;
        }
    }

    return result;
}

FastImportedPrefabLoadResult GeneratedModelRendererArtifactFilesExist(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot)
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
    const auto absolutePath = (projectRoot / std::filesystem::path(assetPath)).lexically_normal();
    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(absolutePath));
    if (!meta.has_value() || !meta->id.IsValid())
        return result;
    auto currentMeta = *meta;
    currentMeta.importerVersion = std::max(
        currentMeta.importerVersion,
        NLS::Core::Assets::GetCurrentImporterVersion(currentMeta.assetType));

    const auto artifactRoot = projectRoot / "Library" / "Artifacts";
    auto manifest = LoadFastManifest(projectRoot, currentMeta.id);
    if (!manifest.has_value() || manifest->sourceAssetId != currentMeta.id)
        return result;
    if (!ManifestDependenciesAreCurrent(*manifest, currentMeta, projectRoot, absolutePath))
        return result;

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

    if (loadMode == ImportedPrefabArtifactLoadMode::ValidateRendererDependencies &&
        assetType == NLS::Core::Assets::AssetType::ModelScene)
    {
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::DependencyScan});
        auto rendererReadiness = ValidateGeneratedModelRendererArtifactsReady(
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
        return result;
    }

    const auto prefabPath = ResolveManifestArtifactPath(projectRoot, artifactRoot, prefabArtifact->artifactPath);
    if (prefabPath.empty())
        return result;

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
        return result;
    result.prefabArtifactBytes = bytes.size();

    const auto container = NLS::Core::Assets::ReadNativeArtifactContainerView(
        bytes,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u);
    if (!container.has_value())
        return result;
    if (container->payloadSize == 0u)
        return result;

    std::vector<NLS::Engine::Assets::PrefabResolvedAsset> resolvedAssets;
    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab)
            continue;

        auto expectedType = ExpectedPrefabResolvedAssetType(artifact.artifactType);

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
    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        payload,
        meta->id,
        std::move(resolvedAssets));
    if (importResult.diagnostics.HasErrors())
        return result;

    auto prefab = std::make_shared<NLS::Engine::Assets::PrefabArtifact>(
        std::move(importResult.artifact));
    prefab->generatedModelPrefab = assetType == NLS::Core::Assets::AssetType::ModelScene;
    result.prefab = std::move(prefab);
    result.rendererDependencyTemplates = BuildImportedPrefabRendererDependencyTemplates(*result.prefab);
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

std::optional<ImportedAssetHandle> ResolveImportedAssetHandleForPreview(
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
    const bool validateRendererDependencies)
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

    if (validateRendererDependencies &&
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

EditorAssetDragDropBridge::EditorAssetDragDropBridge(std::filesystem::path projectAssetsPath)
    : m_projectAssetsPath(std::move(projectAssetsPath))
{
}

std::optional<std::vector<ImportedPrefabRendererDependencyTemplate>>
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
    return std::nullopt;
}

std::optional<UnifiedPrefabLoadKey> TryGetImportedPrefabLoadKeyForArtifact(
    const NLS::Engine::Assets::PrefabArtifact& prefab)
{
    return FindRememberedImportedPrefabLoadedKey(prefab);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
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
        IncludeRendererArtifactStamp(loadMode)
            ? BuildRendererArtifactStamp(*manifest, projectRoot, artifactRoot)
            : std::string {});
}

bool EditorAssetDragDropBridge::PreloadPreparedPrefabHotCache(
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
    return result;
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
    request.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
    request.allowPending = true;
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
    result.dragDrop = AssetDragDropWorkflow().Execute({
        {payloadKind, prefab.assetId, prefabSubAssetKey, nullptr, nullptr, nullptr, {}, &prefab, std::move(sharedPrefab)},
        {DropTargetKind::Hierarchy, &scene, parent, 0u, false},
        sceneAssetId,
        DragDropOperationKind::None,
        nullptr,
        prefabInstanceRegistry,
        {},
        // Keep Scene View mouse release cheap; render/resource systems resolve imported mesh/material references after commit.
        true
    });
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
    auto prefab = database.LoadPrefabArtifactAtPath(assetPath, prefabSubAssetKey);
    if (!prefab.has_value())
        return {};

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
    finalDropRequest.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
    auto fastLoad = LoadUnifiedPrefabShared(finalDropRequest);
    if (fastLoad.prefab)
    {
        return InstantiateImportedAsset(
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
    finalDropRequest.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
    auto fastLoad = LoadUnifiedPrefabShared(finalDropRequest);
    if (fastLoad.prefab)
    {
        return InstantiateImportedAsset(
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
    }
    if (fastLoad.rendererDependencyMissing)
    {
        return MakePendingImportedPrefabResult(
            fastLoad,
            "dragdrop-asset-import-pending",
            "The dragged asset is not imported yet; background preimport must complete before it can be instantiated.");
    }

    result = MakePendingImportedPrefabResult(
        fastLoad,
        "dragdrop-asset-import-pending",
        "The dragged asset is not imported yet; background preimport must complete before it can be instantiated.");

    if (request.scheduleBackgroundTask && request.completion)
    {
        const auto roots = MakeProjectEditorAssetRoots(ProjectRoot());
        auto completion = std::make_shared<std::function<void(EditorAssetDragDropBridgeResult)>>(
            std::move(request.completion));
        const bool scheduled = request.scheduleBackgroundTask(
            [roots, assetPath, completion]() mutable
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
                    const bool imported = importer.SaveAndReimport(assetPath);
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
            });
        if (!scheduled)
        {
            EditorAssetDragDropBridgeResult completionResult;
            completionResult.handled = true;
            completionResult.pendingImport = false;
            completionResult.importSucceeded = false;
            completionResult.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
            completionResult.dragDrop.status = DragDropOperationStatus::Rejected;
            completionResult.dragDrop.diagnostics.push_back({
                "dragdrop-background-task-rejected",
                "The editor background task queue rejected the asset import request."
            });
            if (completion && *completion)
                (*completion)(std::move(completionResult));
        }
    }

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
    finalDropRequest.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
    auto fastLoad = LoadUnifiedPrefabShared(finalDropRequest);
    if (!fastLoad.prefab)
    {
        return MakePendingImportedPrefabResult(
            fastLoad,
            payload.imported == 0u ? "dragdrop-asset-import-pending" : "dragdrop-asset-artifact-missing",
            payload.imported == 0u
                ? "The dragged asset is not imported yet; import must complete before it can be instantiated."
                : "The dragged asset has no committed prefab artifact available for non-blocking instantiation.");
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

    return InstantiateImportedAsset(
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
            "The dragged asset handle no longer matches the cached preview prefab artifact."
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

    if (!IsPreviewPrefabArtifactCurrent(payload, prefab, false))
    {
        result.handled = true;
        result.dragDrop.operation = DragDropOperationKind::InstantiatePrefab;
        result.dragDrop.diagnostics.push_back({
            "dragdrop-cached-artifact-stale",
            "The cached preview prefab artifact is no longer current for this asset."
        });
        return result;
    }

    if (assetType != NLS::Core::Assets::AssetType::ModelScene &&
        assetType != NLS::Core::Assets::AssetType::Prefab)
    {
        return result;
    }

    return InstantiateImportedAsset(
        prefab,
        prefabSubAssetKey,
        assetType,
        scene,
        sceneAssetId,
        prefabInstanceRegistry,
        parent,
        progressTracker,
        assetPath);
}

std::optional<NLS::Engine::Assets::PrefabArtifact> EditorAssetDragDropBridge::TryLoadPreviewPrefabArtifact(
    const EditorAssetDragPayload& payload) const
{
    auto prefab = TryLoadPreviewPrefabArtifactShared(payload);
    if (!prefab)
        return std::nullopt;

    return *prefab;
}

std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> EditorAssetDragDropBridge::TryLoadPreviewPrefabArtifactShared(
    const EditorAssetDragPayload& payload) const
{
    const auto handle = ResolveImportedAssetHandleForPreview(ProjectRoot(), payload);
    if (!handle.has_value())
        return {};

    auto request = MakeUnifiedPrefabLoadRequestForAsset(
        ProjectRoot(),
        handle->assetPath,
        handle->prefabSubAssetKey,
        handle->assetId,
        handle->assetType,
        UnifiedPrefabLoadMode::DragPreview,
        UnifiedPrefabOwnerKind::PreviewScene,
        handle->assetPath);
    request.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
    auto fastLoad = LoadUnifiedPrefabShared(request);
    if (!fastLoad.prefab ||
        fastLoad.prefab->assetId != handle->assetId)
    {
        return {};
    }

    return fastLoad.prefab;
}

bool EditorAssetDragDropBridge::IsPreviewPrefabArtifactCurrent(
    const EditorAssetDragPayload& payload,
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    const bool validateRendererDependencies) const
{
    auto assetPath = NormalizeResourcePath(GetEditorAssetDragPayloadPath(payload));
    if (assetPath.empty())
        return false;

    const auto payloadAssetId = GetEditorAssetDragPayloadAssetId(payload);
    if (!payloadAssetId.IsValid() || prefab.assetId != payloadAssetId)
        return false;

    auto prefabSubAssetKey = GetEditorAssetDragPayloadSubAssetKey(payload);
    const auto assetType = prefab.generatedModelPrefab
        ? NLS::Core::Assets::AssetType::ModelScene
        : NLS::Core::Assets::InferAssetType(ProjectRoot() / assetPath);
    prefabSubAssetKey = NormalizeGeneratedPrefabSubAssetKeyForAssetPath(
        assetPath,
        std::move(prefabSubAssetKey),
        assetType);

    if (!IsImportedPrefabArtifactCurrentForPayload(
        ProjectRoot(),
        payload,
        prefab,
        assetPath,
        prefabSubAssetKey,
        validateRendererDependencies))
    {
        return false;
    }

    const auto rememberedKey = FindRememberedImportedPrefabLoadedKey(prefab);
    if (!rememberedKey.has_value())
        return false;

    auto request = MakeUnifiedPrefabLoadRequestForAsset(
        ProjectRoot(),
        assetPath,
        prefabSubAssetKey,
        prefab.assetId,
        assetType,
        UnifiedPrefabLoadMode::DragPreview,
        UnifiedPrefabOwnerKind::PreviewScene,
        assetPath);
    if (!validateRendererDependencies)
        request.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;

    const auto currentKey = BuildUnifiedPrefabLoadKey(request);
    return currentKey.has_value() &&
        currentKey->runtimeCacheIdentity == rememberedKey->runtimeCacheIdentity;
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
