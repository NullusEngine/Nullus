#include "Assets/EditorAssetDragDropBridge.h"

#include "Assets/AssetMeta.h"
#include "Assets/AssetImporterFacade.h"
#include "Assets/EditorAssetManifestJson.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/NativeArtifactContainer.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <Json/json.hpp>
#include <memory>
#include <system_error>
#include <vector>

namespace NLS::Editor::Assets
{
namespace
{
struct FastImportedPrefabLoadResult
{
    std::optional<NLS::Engine::Assets::PrefabArtifact> prefab;
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

struct ImportedAssetHandle
{
    std::string assetPath;
    std::string prefabSubAssetKey;
    NLS::Core::Assets::AssetType assetType = NLS::Core::Assets::AssetType::Unknown;
    NLS::Core::Assets::AssetId assetId;
};

std::string DefaultGeneratedPrefabSubAssetKeyForAssetPath(const std::string& assetPath)
{
    return "prefab:" + std::filesystem::path(assetPath).stem().generic_string();
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
        "external-texture-build-pipeline",
        "1");
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

    const auto sourceAssetId = artifactRoot.filename();
    if (sourceAssetId.empty())
        return std::nullopt;

    std::vector<std::filesystem::path> parts;
    for (const auto& part : absoluteArtifactPath.lexically_normal())
        parts.push_back(part);

    for (size_t index = 0u; index + 1u < parts.size(); ++index)
    {
        if (parts[index + 1u] != sourceAssetId)
            continue;

        const auto artifactDirectory = parts[index].generic_string();
        if (artifactDirectory != "Artifacts" && artifactDirectory != "ArtifactStaging")
            continue;

        std::filesystem::path relative;
        for (size_t relativeIndex = index + 2u; relativeIndex < parts.size(); ++relativeIndex)
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

std::optional<NLS::Core::Assets::ArtifactManifest> LoadFastManifest(
    const std::filesystem::path& manifestPath)
{
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input)
        return std::nullopt;

    const auto root = nlohmann::json::parse(input, nullptr, false);
    return ParseArtifactManifestJson(root, true);
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path)
{
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

    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
        bytes,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    return container.has_value() && !container->payload.empty();
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
    default:
        return {};
    }
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

    const auto artifactRoot = projectRoot / "Library" / "Artifacts" / currentMeta.id.ToString();
    auto manifest = LoadFastManifest(artifactRoot / "manifest.json");
    if (!manifest.has_value() || manifest->sourceAssetId != currentMeta.id)
        return result;
    if (!ManifestDependenciesAreCurrent(*manifest, currentMeta, projectRoot, absolutePath))
        return result;

    if (assetType == NLS::Core::Assets::AssetType::ModelScene &&
        loadMode == ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles)
    {
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

    const auto bytes = ReadAllBytes(prefabPath);
    if (bytes.empty())
        return result;

    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
        bytes,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u);
    if (!container.has_value())
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

    const std::string payload(container->payload.begin(), container->payload.end());
    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        payload,
        meta->id,
        std::move(resolvedAssets));
    if (importResult.diagnostics.HasErrors())
        return result;

    auto prefab = std::move(importResult.artifact);
    prefab.generatedModelPrefab = assetType == NLS::Core::Assets::AssetType::ModelScene;
    result.prefab = std::move(prefab);
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

std::optional<ImportedAssetHandle> ResolveImportedAssetHandleForPreview(
    const std::filesystem::path& projectRoot,
    const EditorAssetDragPayload& payload)
{
    auto assetPath = NormalizeEditorAssetPath(GetEditorAssetDragPayloadPath(payload));
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
    if (payload.generatedModelPrefab != 0u ||
        prefabSubAssetKey.empty())
    {
        prefabSubAssetKey = DefaultGeneratedPrefabSubAssetKeyForAssetPath(assetPath);
    }

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

    const auto artifactRoot = projectRoot / "Library" / "Artifacts" / currentMeta.id.ToString();
    const auto manifest = LoadFastManifest(artifactRoot / "manifest.json");
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
    else if (currentMeta.assetType == NLS::Core::Assets::AssetType::ModelScene)
    {
        auto rendererReadiness = GeneratedModelRendererArtifactFilesExist(
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

}

EditorAssetDragDropBridge::EditorAssetDragDropBridge(std::filesystem::path projectAssetsPath)
    : m_projectAssetsPath(std::move(projectAssetsPath))
{
}

std::filesystem::path EditorAssetDragDropBridge::ProjectRoot() const
{
    auto assetsPath = m_projectAssetsPath.lexically_normal();
    while (!assetsPath.empty() && !assetsPath.has_filename())
        assetsPath = assetsPath.parent_path();
    return assetsPath.parent_path();
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
    EditorAssetDragDropBridgeResult result;

    result.handled = true;
    (void)progressTracker;
    (void)progressLabel;

    const auto payloadKind = assetType == NLS::Core::Assets::AssetType::ModelScene
        ? DragPayloadKind::GeneratedModelPrefabAsset
        : DragPayloadKind::PrefabAsset;
    result.dragDrop = AssetDragDropWorkflow().Execute({
        {payloadKind, prefab.assetId, prefabSubAssetKey, &prefab},
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

    auto fastLoad = LoadImportedPrefabFast(
        ProjectRoot(),
        assetPath,
        prefabSubAssetKey,
        assetType,
        ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles);
    if (fastLoad.prefab.has_value())
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
            assetPath);
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

    auto fastLoad = LoadImportedPrefabFast(
        ProjectRoot(),
        assetPath,
        prefabSubAssetKey,
        assetType,
        ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles);
    if (fastLoad.prefab.has_value())
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
            assetPath);
    }
    if (fastLoad.rendererDependencyMissing)
    {
        return MakePendingImportedPrefabResult(
            fastLoad,
            "dragdrop-asset-import-pending",
            "The dragged asset is not imported yet; background preimport must complete before it can be instantiated.");
    }

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(ProjectRoot()));
    if (!database.Refresh())
        return result;

    if (database.LoadSubAssetAtPath(assetPath, prefabSubAssetKey).has_value())
    {
        return InstantiateImportedAsset(
            database,
            assetPath,
            prefabSubAssetKey,
            assetType,
            scene,
            request.sceneAssetId,
            request.prefabInstanceRegistry,
            request.parent,
            request.progressTracker,
            assetPath);
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
    if (payload.generatedModelPrefab != 0u ||
        prefabSubAssetKey.empty())
    {
        prefabSubAssetKey = DefaultGeneratedPrefabSubAssetKey(assetPath);
    }

    if (assetType != NLS::Core::Assets::AssetType::ModelScene &&
        assetType != NLS::Core::Assets::AssetType::Prefab)
    {
        return result;
    }

    auto fastLoad = LoadImportedPrefabFast(
        ProjectRoot(),
        assetPath,
        prefabSubAssetKey,
        assetType,
        ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles);
    if (!fastLoad.prefab.has_value())
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
        assetPath);
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
    if (prefab.generatedModelPrefab || prefabSubAssetKey.empty())
        prefabSubAssetKey = DefaultGeneratedPrefabSubAssetKey(assetPath);

    if (!IsImportedPrefabArtifactCurrentForPayload(ProjectRoot(), payload, prefab, assetPath, prefabSubAssetKey, false))
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
    const auto handle = ResolveImportedAssetHandleForPreview(ProjectRoot(), payload);
    if (!handle.has_value())
        return std::nullopt;

    auto fastLoad = LoadImportedPrefabFast(
        ProjectRoot(),
        handle->assetPath,
        handle->prefabSubAssetKey,
        handle->assetType,
        ImportedPrefabArtifactLoadMode::RequireRendererArtifactFiles);
    if (!fastLoad.prefab.has_value() ||
        fastLoad.prefab->assetId != handle->assetId)
    {
        return std::nullopt;
    }

    return std::move(fastLoad.prefab);
}
}
