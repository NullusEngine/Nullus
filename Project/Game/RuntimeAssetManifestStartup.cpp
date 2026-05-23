#include "RuntimeAssetManifestStartup.h"

#include "Core/ResourceManagement/MaterialManager.h"
#include "Guid.h"

#include <Json/json.hpp>

#include <algorithm>
#include <fstream>
#include <queue>
#include <unordered_set>

namespace NLS::Game::RuntimeAssets
{
namespace
{
    template <typename T>
    std::optional<T> JsonValue(const nlohmann::json& json, const char* key)
    {
        const auto found = json.find(key);
        if (found == json.end() || found->is_null())
            return std::nullopt;

        try
        {
            return found->get<T>();
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<NLS::Core::Assets::AssetId> JsonAssetId(const nlohmann::json& json, const char* key)
    {
        const auto value = JsonValue<std::string>(json, key);
        if (!value.has_value())
            return std::nullopt;

        const auto parsed = NLS::Guid::TryParse(*value);
        if (!parsed.has_value())
            return std::nullopt;

        return NLS::Core::Assets::AssetId(*parsed);
    }

    std::vector<NLS::Engine::Assets::RuntimeAssetRef> ParseRuntimeAssetRefs(const nlohmann::json& json)
    {
        std::vector<NLS::Engine::Assets::RuntimeAssetRef> result;
        if (!json.is_array())
            return result;

        for (const auto& item : json)
        {
            if (!item.is_object())
                continue;

            const auto assetId = JsonAssetId(item, "assetId");
            const auto subAssetKey = JsonValue<std::string>(item, "subAssetKey");
            if (!assetId.has_value() || !subAssetKey.has_value())
                continue;

            result.push_back({*assetId, *subAssetKey});
        }
        return result;
    }

    std::optional<NLS::Core::Assets::ArtifactType> JsonArtifactType(const nlohmann::json& json, const char* key)
    {
        const auto found = json.find(key);
        if (found == json.end() || found->is_null())
            return std::nullopt;

        if (found->is_number_integer())
            return static_cast<NLS::Core::Assets::ArtifactType>(found->get<int>());

        if (!found->is_string())
            return std::nullopt;

        const auto value = found->get<std::string>();
        using NLS::Core::Assets::ArtifactType;
        if (value == "model") return ArtifactType::Model;
        if (value == "mesh") return ArtifactType::Mesh;
        if (value == "material") return ArtifactType::Material;
        if (value == "texture") return ArtifactType::Texture;
        if (value == "skeleton") return ArtifactType::Skeleton;
        if (value == "skin") return ArtifactType::Skin;
        if (value == "animation" || value == "animationClip") return ArtifactType::AnimationClip;
        if (value == "morph" || value == "morphTarget" || value == "morph-target") return ArtifactType::MorphTarget;
        if (value == "prefab") return ArtifactType::Prefab;
        if (value == "scene") return ArtifactType::Scene;
        if (value == "shader") return ArtifactType::Shader;
        if (value == "audio") return ArtifactType::Audio;
        return ArtifactType::Unknown;
    }

    std::optional<NLS::Engine::Assets::RuntimeAssetManifestEntry> ParseRuntimeAssetEntry(const nlohmann::json& json)
    {
        if (!json.is_object())
            return std::nullopt;

        const auto assetId = JsonAssetId(json, "assetId");
        const auto subAssetKey = JsonValue<std::string>(json, "subAssetKey");
        if (!assetId.has_value() || !subAssetKey.has_value())
            return std::nullopt;

        NLS::Engine::Assets::RuntimeAssetManifestEntry entry;
        entry.assetId = *assetId;
        entry.subAssetKey = *subAssetKey;
        entry.artifactType = JsonArtifactType(json, "artifactType").value_or(NLS::Core::Assets::ArtifactType::Unknown);
        entry.loaderId = JsonValue<std::string>(json, "loaderId").value_or(std::string {});
        entry.artifactPath = JsonValue<std::string>(json, "artifactPath").value_or(std::string {});
        entry.contentHash = JsonValue<std::string>(json, "contentHash").value_or(std::string {});

        const auto dependencies = json.find("dependencies");
        if (dependencies != json.end())
            entry.dependencies = ParseRuntimeAssetRefs(*dependencies);

        return entry;
    }

    std::optional<NLS::Engine::Assets::RuntimePrefabManifestEntry> ParseRuntimePrefabEntry(const nlohmann::json& json)
    {
        if (!json.is_object())
            return std::nullopt;

        const auto prefabJson = json.find("prefab");
        if (prefabJson == json.end())
            return std::nullopt;

        const auto prefabRefs = ParseRuntimeAssetRefs(nlohmann::json::array({*prefabJson}));
        if (prefabRefs.empty())
            return std::nullopt;

        NLS::Engine::Assets::RuntimePrefabManifestEntry entry;
        entry.prefab = prefabRefs.front();
        entry.graphArtifactPath = JsonValue<std::string>(json, "graphArtifactPath").value_or(std::string {});

        const auto dependencies = json.find("dependencies");
        if (dependencies != json.end())
            entry.dependencies = ParseRuntimeAssetRefs(*dependencies);

        return entry;
    }

    std::optional<NLS::Engine::Assets::RuntimeAssetPackEntry> ParseRuntimeAssetPackEntry(const nlohmann::json& json)
    {
        if (!json.is_object())
            return std::nullopt;

        const auto referenceJson = json.find("reference");
        if (referenceJson == json.end())
            return std::nullopt;

        const auto references = ParseRuntimeAssetRefs(nlohmann::json::array({*referenceJson}));
        if (references.empty())
            return std::nullopt;

        NLS::Engine::Assets::RuntimeAssetPackEntry entry;
        entry.reference = references.front();
        entry.artifactType = JsonArtifactType(json, "artifactType").value_or(NLS::Core::Assets::ArtifactType::Unknown);
        entry.loaderId = JsonValue<std::string>(json, "loaderId").value_or(std::string {});
        entry.artifactPath = JsonValue<std::string>(json, "artifactPath").value_or(std::string {});
        entry.contentHash = JsonValue<std::string>(json, "contentHash").value_or(std::string {});

        const auto dependencies = json.find("dependencies");
        if (dependencies != json.end())
            entry.dependencies = ParseRuntimeAssetRefs(*dependencies);

        return entry;
    }

    std::optional<NLS::Engine::Assets::RuntimeAssetPack> ParseRuntimeAssetPack(const nlohmann::json& json)
    {
        if (!json.is_object())
            return std::nullopt;

        NLS::Engine::Assets::RuntimeAssetPack pack;
        pack.packName = JsonValue<std::string>(json, "packName").value_or(std::string {});
        pack.packVariant = JsonValue<std::string>(json, "packVariant").value_or(std::string {});

        const auto entries = json.find("entries");
        if (entries != json.end() && entries->is_array())
        {
            for (const auto& entryJson : *entries)
            {
                auto entry = ParseRuntimeAssetPackEntry(entryJson);
                if (entry.has_value())
                    pack.entries.push_back(std::move(*entry));
            }
        }

        return pack;
    }

    std::optional<NLS::Engine::Assets::RuntimeAssetManifest> ParseRuntimeAssetManifest(const nlohmann::json& json)
    {
        if (!json.is_object())
            return std::nullopt;

        NLS::Engine::Assets::RuntimeAssetManifest manifest;
        manifest.schemaVersion = JsonValue<uint32_t>(json, "schemaVersion").value_or(1u);
        manifest.targetPlatform = JsonValue<std::string>(json, "targetPlatform").value_or(std::string {});

        if (const auto roots = json.find("roots"); roots != json.end())
            manifest.roots = ParseRuntimeAssetRefs(*roots);

        const auto entries = json.find("entries");
        if (entries == json.end() || !entries->is_array())
            return std::nullopt;

        for (const auto& entryJson : *entries)
        {
            auto entry = ParseRuntimeAssetEntry(entryJson);
            if (entry.has_value())
                manifest.entries.push_back(std::move(*entry));
        }

        if (const auto prefabEntries = json.find("prefabEntries");
            prefabEntries != json.end() && prefabEntries->is_array())
        {
            for (const auto& prefabEntryJson : *prefabEntries)
            {
                auto prefabEntry = ParseRuntimePrefabEntry(prefabEntryJson);
                if (prefabEntry.has_value())
                    manifest.prefabEntries.push_back(std::move(*prefabEntry));
            }
        }

        if (const auto assetPacks = json.find("assetPacks");
            assetPacks != json.end() && assetPacks->is_array())
        {
            for (const auto& assetPackJson : *assetPacks)
            {
                auto assetPack = ParseRuntimeAssetPack(assetPackJson);
                if (assetPack.has_value())
                    manifest.assetPacks.push_back(std::move(*assetPack));
            }
        }

        if (manifest.entries.empty())
            return std::nullopt;

        return manifest;
    }

    bool IsMaterialEntry(
        const NLS::Core::Assets::ArtifactType artifactType,
        const std::string& loaderId)
    {
        return artifactType == NLS::Core::Assets::ArtifactType::Material || loaderId == "material";
    }

    bool ContainsPackSelection(
        const std::vector<std::pair<std::string, std::string>>& packs,
        const NLS::Engine::Assets::RuntimeAssetPack& pack)
    {
        return std::any_of(
            packs.begin(),
            packs.end(),
            [&pack](const auto& selection)
            {
                return selection.first == pack.packName && selection.second == pack.packVariant;
            });
    }

    void AddUniqueReference(
        std::vector<NLS::Engine::Assets::RuntimeAssetRef>& references,
        const NLS::Engine::Assets::RuntimeAssetRef& reference)
    {
        if (std::find(references.begin(), references.end(), reference) == references.end())
            references.push_back(reference);
    }

    std::vector<NLS::Engine::Assets::RuntimeAssetRef> ResolvePrewarmRoots(
        const NLS::Engine::Assets::RuntimeAssetManifest& manifest,
        const RuntimeMaterialPrewarmOptions& options)
    {
        std::vector<NLS::Engine::Assets::RuntimeAssetRef> roots = options.roots;

        for (const auto& pack : manifest.assetPacks)
        {
            if (!ContainsPackSelection(options.assetPacks, pack))
                continue;

            for (const auto& entry : pack.entries)
                AddUniqueReference(roots, entry.reference);
        }

        if (roots.empty() && options.assetPacks.empty())
            roots = manifest.roots;

        return roots;
    }
}

std::filesystem::path ResolveRuntimeAssetManifestPathForProjectSettings(
    const std::filesystem::path& projectSettingsPath)
{
    if (!projectSettingsPath.empty())
    {
        const auto projectRoot = projectSettingsPath.parent_path();
        if (!projectRoot.empty())
            return projectRoot / "Library" / "RuntimeAssetManifest.json";
    }

    return std::filesystem::current_path() / "Data" / "RuntimeAssetManifest.json";
}

std::optional<NLS::Engine::Assets::RuntimeAssetDatabase> LoadRuntimeAssetDatabaseForProjectSettings(
    const std::filesystem::path& projectSettingsPath)
{
    const auto manifestPath = ResolveRuntimeAssetManifestPathForProjectSettings(projectSettingsPath);
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input)
        return std::nullopt;

    const auto json = nlohmann::json::parse(input, nullptr, false);
    auto manifest = ParseRuntimeAssetManifest(json);
    if (!manifest.has_value())
        return std::nullopt;

    return NLS::Engine::Assets::RuntimeAssetDatabase(std::move(*manifest));
}

size_t PrewarmRuntimeMaterialAssets(
    const NLS::Engine::Assets::RuntimeAssetDatabase& runtimeDatabase,
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    const RuntimeMaterialPrewarmOptions& options)
{
    size_t loadedCount = 0u;
    std::queue<NLS::Engine::Assets::RuntimeAssetRef> pending;
    std::unordered_set<NLS::Engine::Assets::RuntimeAssetRef> visited;

    for (const auto& root : ResolvePrewarmRoots(runtimeDatabase.GetManifest(), options))
        pending.push(root);

    while (!pending.empty())
    {
        const auto reference = pending.front();
        pending.pop();

        if (!visited.insert(reference).second)
            continue;

        const auto* entry = runtimeDatabase.Resolve(reference);
        if (entry == nullptr)
            continue;

        for (const auto& dependency : entry->dependencies)
            pending.push(dependency);

        if (entry->artifactPath.empty() || !IsMaterialEntry(entry->artifactType, entry->loaderId))
            continue;

        if (materialManager.LoadArtifactWithoutTextures(entry->artifactPath) != nullptr)
            ++loadedCount;
    }

    return loadedCount;
}
}
