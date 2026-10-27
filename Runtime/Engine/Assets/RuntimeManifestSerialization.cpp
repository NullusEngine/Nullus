#include "Engine/Assets/RuntimeManifestSerialization.h"

#include <Json/json.hpp>

#include <algorithm>

namespace NLS::Engine::Assets
{
namespace
{
constexpr uint32_t kRuntimeManifestSchemaVersion = 1u;

nlohmann::json ReferenceToJson(const RuntimeAssetRef& reference)
{
    nlohmann::json json;
    json["assetId"] = reference.assetId.ToString();
    json["subAssetKey"] = reference.subAssetKey;
    return json;
}

std::optional<RuntimeAssetRef> ReferenceFromJson(const nlohmann::json& json)
{
    const auto guid = NLS::Guid::TryParse(json.value("assetId", std::string {}));
    if (!guid.has_value())
        return std::nullopt;
    RuntimeAssetRef reference;
    reference.assetId = NLS::Core::Assets::AssetId(*guid);
    reference.subAssetKey = json.value("subAssetKey", std::string {});
    return reference;
}
} // namespace

std::string SerializeRuntimeAssetManifest(const RuntimeAssetManifest& manifest)
{
    nlohmann::json json;
    json["schemaVersion"] = manifest.schemaVersion;
    json["targetPlatform"] = manifest.targetPlatform;

    nlohmann::json roots = nlohmann::json::array();
    for (const auto& root : manifest.roots)
        roots.push_back(ReferenceToJson(root));
    json["roots"] = std::move(roots);

    nlohmann::json entries = nlohmann::json::array();
    for (const auto& entry : manifest.entries)
    {
        nlohmann::json dependencies = nlohmann::json::array();
        for (const auto& dependency : entry.dependencies)
            dependencies.push_back(ReferenceToJson(dependency));
        entries.push_back({
            {"assetId", entry.assetId.ToString()},
            {"subAssetKey", entry.subAssetKey},
            {"artifactType", static_cast<uint32_t>(entry.artifactType)},
            {"loaderId", entry.loaderId},
            {"artifactPath", entry.artifactPath},
            {"contentHash", entry.contentHash},
            {"dependencies", std::move(dependencies)}});
    }
    json["entries"] = std::move(entries);

    nlohmann::json prefabs = nlohmann::json::array();
    for (const auto& prefab : manifest.prefabEntries)
    {
        nlohmann::json dependencies = nlohmann::json::array();
        for (const auto& dependency : prefab.dependencies)
            dependencies.push_back(ReferenceToJson(dependency));
        prefabs.push_back({
            {"prefab", ReferenceToJson(prefab.prefab)},
            {"graphArtifactPath", prefab.graphArtifactPath},
            {"dependencies", std::move(dependencies)}});
    }
    json["prefabEntries"] = std::move(prefabs);

    nlohmann::json packs = nlohmann::json::array();
    for (const auto& pack : manifest.assetPacks)
    {
        nlohmann::json packEntries = nlohmann::json::array();
        for (const auto& entry : pack.entries)
        {
            nlohmann::json dependencies = nlohmann::json::array();
            for (const auto& dependency : entry.dependencies)
                dependencies.push_back(ReferenceToJson(dependency));
            packEntries.push_back({
                {"reference", ReferenceToJson(entry.reference)},
                {"artifactType", static_cast<uint32_t>(entry.artifactType)},
                {"loaderId", entry.loaderId},
                {"artifactPath", entry.artifactPath},
                {"contentHash", entry.contentHash},
                {"dependencies", std::move(dependencies)}});
        }
        packs.push_back({
            {"packName", pack.packName},
            {"packVariant", pack.packVariant},
            {"entries", std::move(packEntries)}});
    }
    json["assetPacks"] = std::move(packs);

    return json.dump(2);
}

std::optional<RuntimeAssetManifest> DeserializeRuntimeAssetManifest(
    const std::string& json,
    std::string* diagnostics)
{
    try
    {
        const auto parsed = nlohmann::json::parse(json);
        RuntimeAssetManifest manifest;
        manifest.schemaVersion = parsed.value("schemaVersion", kRuntimeManifestSchemaVersion);
        manifest.targetPlatform = parsed.value("targetPlatform", std::string {});

        if (parsed.contains("roots") && parsed["roots"].is_array())
        {
            for (const auto& root : parsed["roots"])
            {
                if (auto reference = ReferenceFromJson(root); reference.has_value())
                    manifest.roots.push_back(std::move(*reference));
            }
        }
        if (parsed.contains("entries") && parsed["entries"].is_array())
        {
            for (const auto& entryJson : parsed["entries"])
            {
                const auto guid = NLS::Guid::TryParse(entryJson.value("assetId", std::string {}));
                if (!guid.has_value())
                    continue;
                RuntimeAssetManifestEntry entry;
                entry.assetId = NLS::Core::Assets::AssetId(*guid);
                entry.subAssetKey = entryJson.value("subAssetKey", std::string {});
                entry.artifactType = static_cast<NLS::Core::Assets::ArtifactType>(
                    entryJson.value("artifactType", 0u));
                entry.loaderId = entryJson.value("loaderId", std::string {});
                entry.artifactPath = entryJson.value("artifactPath", std::string {});
                entry.contentHash = entryJson.value("contentHash", std::string {});
                if (entryJson.contains("dependencies") && entryJson["dependencies"].is_array())
                {
                    for (const auto& dependency : entryJson["dependencies"])
                    {
                        if (auto reference = ReferenceFromJson(dependency); reference.has_value())
                            entry.dependencies.push_back(std::move(*reference));
                    }
                }
                manifest.entries.push_back(std::move(entry));
            }
        }
        if (parsed.contains("prefabEntries") && parsed["prefabEntries"].is_array())
        {
            for (const auto& prefabJson : parsed["prefabEntries"])
            {
                RuntimePrefabManifestEntry prefab;
                if (prefabJson.contains("prefab"))
                {
                    if (auto reference = ReferenceFromJson(prefabJson["prefab"]); reference.has_value())
                        prefab.prefab = std::move(*reference);
                }
                prefab.graphArtifactPath = prefabJson.value("graphArtifactPath", std::string {});
                if (prefabJson.contains("dependencies") && prefabJson["dependencies"].is_array())
                {
                    for (const auto& dependency : prefabJson["dependencies"])
                    {
                        if (auto reference = ReferenceFromJson(dependency); reference.has_value())
                            prefab.dependencies.push_back(std::move(*reference));
                    }
                }
                manifest.prefabEntries.push_back(std::move(prefab));
            }
        }
        if (parsed.contains("assetPacks") && parsed["assetPacks"].is_array())
        {
            for (const auto& packJson : parsed["assetPacks"])
            {
                RuntimeAssetPack pack;
                pack.packName = packJson.value("packName", std::string {});
                pack.packVariant = packJson.value("packVariant", std::string {});
                if (packJson.contains("entries") && packJson["entries"].is_array())
                {
                    for (const auto& entryJson : packJson["entries"])
                    {
                        RuntimeAssetPackEntry entry;
                        if (entryJson.contains("reference"))
                        {
                            if (auto reference = ReferenceFromJson(entryJson["reference"]); reference.has_value())
                                entry.reference = std::move(*reference);
                        }
                        entry.artifactType = static_cast<NLS::Core::Assets::ArtifactType>(
                            entryJson.value("artifactType", 0u));
                        entry.loaderId = entryJson.value("loaderId", std::string {});
                        entry.artifactPath = entryJson.value("artifactPath", std::string {});
                        entry.contentHash = entryJson.value("contentHash", std::string {});
                        if (entryJson.contains("dependencies") && entryJson["dependencies"].is_array())
                        {
                            for (const auto& dependency : entryJson["dependencies"])
                            {
                                if (auto reference = ReferenceFromJson(dependency); reference.has_value())
                                    entry.dependencies.push_back(std::move(*reference));
                            }
                        }
                        pack.entries.push_back(std::move(entry));
                    }
                }
                manifest.assetPacks.push_back(std::move(pack));
            }
        }

        if (manifest.entries.empty())
        {
            if (diagnostics != nullptr)
                *diagnostics = "runtime manifest contains no entries";
            return std::nullopt;
        }
        return manifest;
    }
    catch (const std::exception& exception)
    {
        if (diagnostics != nullptr)
            *diagnostics = exception.what();
        return std::nullopt;
    }
}
} // namespace NLS::Engine::Assets
