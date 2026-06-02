#pragma once

#include "Assets/ArtifactManifest.h"
#include "Guid.h"

#include <Json/json.hpp>

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <string>

namespace NLS::Editor::Assets
{
inline std::optional<std::string> JsonString(const nlohmann::json& object, const char* key)
{
    if (!object.is_object())
        return std::nullopt;

    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return std::nullopt;

    return it->get<std::string>();
}

inline std::optional<std::string> JsonStringOrDefault(
    const nlohmann::json& object,
    const char* key,
    std::string defaultValue = {})
{
    if (!object.is_object())
        return std::nullopt;

    const auto it = object.find(key);
    if (it == object.end())
        return defaultValue;
    if (!it->is_string())
        return std::nullopt;

    return it->get<std::string>();
}

inline std::optional<uint32_t> JsonUInt(const nlohmann::json& object, const char* key)
{
    if (!object.is_object())
        return std::nullopt;

    const auto it = object.find(key);
    if (it == object.end())
        return std::nullopt;

    if (it->is_number_unsigned())
    {
        const auto value = it->get<uint64_t>();
        if (value <= std::numeric_limits<uint32_t>::max())
            return static_cast<uint32_t>(value);
        return std::nullopt;
    }

    if (it->is_number_integer())
    {
        const auto value = it->get<int64_t>();
        if (value >= 0 && value <= static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
            return static_cast<uint32_t>(value);
    }

    return std::nullopt;
}

inline std::optional<uint32_t> JsonUIntOrDefault(
    const nlohmann::json& object,
    const char* key,
    const uint32_t defaultValue)
{
    if (!object.is_object())
        return std::nullopt;

    const auto it = object.find(key);
    if (it == object.end())
        return defaultValue;

    return JsonUInt(object, key);
}

inline NLS::Core::Assets::ArtifactType ArtifactTypeFromManifestKey(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

    using NLS::Core::Assets::ArtifactType;
    if (value == "model")
        return ArtifactType::Model;
    if (value == "mesh")
        return ArtifactType::Mesh;
    if (value == "material")
        return ArtifactType::Material;
    if (value == "texture")
        return ArtifactType::Texture;
    if (value == "skeleton")
        return ArtifactType::Skeleton;
    if (value == "skin")
        return ArtifactType::Skin;
    if (value == "animation" || value == "animation-clip")
        return ArtifactType::AnimationClip;
    if (value == "morph-target")
        return ArtifactType::MorphTarget;
    if (value == "prefab")
        return ArtifactType::Prefab;
    if (value == "scene")
        return ArtifactType::Scene;
    if (value == "shader")
        return ArtifactType::Shader;
    if (value == "audio")
        return ArtifactType::Audio;
    return ArtifactType::Unknown;
}

inline NLS::Core::Assets::AssetDependencyKind DependencyKindFromManifestKey(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

    using NLS::Core::Assets::AssetDependencyKind;
    if (value == "source-asset-guid")
        return AssetDependencyKind::SourceAssetGuid;
    if (value == "imported-artifact")
        return AssetDependencyKind::ImportedArtifact;
    if (value == "path-to-guid-mapping")
        return AssetDependencyKind::PathToGuidMapping;
    if (value == "build-target")
        return AssetDependencyKind::BuildTarget;
    if (value == "importer-version")
        return AssetDependencyKind::ImporterVersion;
    if (value == "postprocessor-version")
        return AssetDependencyKind::PostprocessorVersion;
    if (value == "prefab-base")
        return AssetDependencyKind::PrefabBase;
    if (value == "nested-prefab")
        return AssetDependencyKind::NestedPrefab;
    if (value == "prefab-override-target")
        return AssetDependencyKind::PrefabOverrideTarget;
    if (value == "runtime-component-capability")
        return AssetDependencyKind::RuntimeComponentCapability;
    if (value == "raw-package-file")
        return AssetDependencyKind::RawPackageFile;
    return AssetDependencyKind::SourceFileHash;
}

inline std::optional<NLS::Core::Assets::ArtifactManifest> ParseArtifactManifestJson(
    const nlohmann::json& root,
    const bool requireSubAssets)
{
    if (!root.is_object())
        return std::nullopt;

    const auto sourceAssetIdText = JsonString(root, "sourceAssetId");
    if (!sourceAssetIdText.has_value())
        return std::nullopt;

    const auto sourceAssetGuid = NLS::Guid::TryParse(*sourceAssetIdText);
    if (!sourceAssetGuid.has_value())
        return std::nullopt;

    const auto importerId = JsonStringOrDefault(root, "importerId");
    const auto importerVersion = JsonUIntOrDefault(root, "importerVersion", 1u);
    const auto targetPlatform = JsonString(root, "targetPlatform");
    const auto primarySubAssetKey = JsonStringOrDefault(root, "primarySubAssetKey");
    if (!importerId.has_value() ||
        !importerVersion.has_value() ||
        !primarySubAssetKey.has_value())
    {
        return std::nullopt;
    }

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = NLS::Core::Assets::AssetId(*sourceAssetGuid);
    manifest.importerId = *importerId;
    manifest.importerVersion = *importerVersion;
    manifest.primarySubAssetKey = *primarySubAssetKey;

    const auto subAssets = root.find("subAssets");
    if (subAssets == root.end())
    {
        if (requireSubAssets)
            return std::nullopt;
    }
    else
    {
        if (!subAssets->is_array())
            return std::nullopt;

        for (const auto& artifactJson : *subAssets)
        {
            if (!artifactJson.is_object())
                return std::nullopt;

            const auto artifactSourceAssetId = JsonString(artifactJson, "sourceAssetId");
            if (!artifactSourceAssetId.has_value())
                return std::nullopt;

            const auto artifactGuid = NLS::Guid::TryParse(*artifactSourceAssetId);
            if (!artifactGuid.has_value())
                return std::nullopt;

            const auto subAssetKey = JsonStringOrDefault(artifactJson, "subAssetKey");
            const auto artifactType = JsonStringOrDefault(artifactJson, "artifactType");
            const auto loaderId = JsonStringOrDefault(artifactJson, "loaderId");
            const auto artifactTargetPlatform = JsonStringOrDefault(artifactJson, "targetPlatform");
            const auto artifactPath = JsonStringOrDefault(artifactJson, "artifactPath");
            const auto contentHash = JsonStringOrDefault(artifactJson, "contentHash");
            if (!subAssetKey.has_value() ||
                !artifactType.has_value() ||
                !loaderId.has_value() ||
                !artifactTargetPlatform.has_value() ||
                !artifactPath.has_value() ||
                !contentHash.has_value())
            {
                return std::nullopt;
            }

            manifest.subAssets.push_back({
                NLS::Core::Assets::AssetId(*artifactGuid),
                *subAssetKey,
                ArtifactTypeFromManifestKey(*artifactType),
                *loaderId,
                *artifactTargetPlatform,
                *artifactPath,
                *contentHash
            });
        }
    }

    if (targetPlatform.has_value())
    {
        manifest.targetPlatform = *targetPlatform;
        if (manifest.targetPlatform.empty())
            return std::nullopt;
    }
    else if (!manifest.subAssets.empty())
    {
        const auto& inferredTargetPlatform = manifest.subAssets.front().targetPlatform;
        if (inferredTargetPlatform.empty())
            return std::nullopt;

        for (const auto& artifact : manifest.subAssets)
        {
            if (artifact.targetPlatform != inferredTargetPlatform)
                return std::nullopt;
        }

        manifest.targetPlatform = inferredTargetPlatform;
    }
    else
    {
        return std::nullopt;
    }

    if (const auto dependencies = root.find("dependencies");
        dependencies != root.end())
    {
        if (!dependencies->is_array())
            return std::nullopt;

        for (const auto& dependencyJson : *dependencies)
        {
            if (!dependencyJson.is_object())
                return std::nullopt;

            const auto kind = JsonStringOrDefault(dependencyJson, "kind");
            const auto value = JsonStringOrDefault(dependencyJson, "value");
            const auto hashOrVersion = JsonStringOrDefault(dependencyJson, "hashOrVersion");
            if (!kind.has_value() || !value.has_value() || !hashOrVersion.has_value())
                return std::nullopt;

            manifest.dependencies.push_back({
                DependencyKindFromManifestKey(*kind),
                *value,
                *hashOrVersion
            });
        }
    }

    return manifest;
}
}
