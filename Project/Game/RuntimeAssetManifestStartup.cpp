#include "RuntimeAssetManifestStartup.h"
#include <Core/ServiceLocator.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Rendering/Resources/Loaders/ShaderPassArtifactRegistry.h>

#include "Assets/ArtifactDatabase.h"
#include "Assets/ContentSource.h"
#include "Assets/NpakArchive.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Engine/Assets/RuntimeManifestSerialization.h"
#include "Guid.h"
#include "Platform/Process/Process.h"
#include "Version.h"

#include <algorithm>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_set>

namespace NLS::Game::RuntimeAssets
{
namespace
{
    std::string LoaderIdForArtifactType(const NLS::Core::Assets::ArtifactType artifactType)
    {
        using NLS::Core::Assets::ArtifactType;
        switch (artifactType)
        {
        case ArtifactType::Model: return "model";
        case ArtifactType::Mesh: return "mesh";
        case ArtifactType::Material: return "material";
        case ArtifactType::Texture: return "texture";
        case ArtifactType::Skeleton: return "skeleton";
        case ArtifactType::Skin: return "skin";
        case ArtifactType::AnimationClip: return "animationClip";
        case ArtifactType::MorphTarget: return "morphTarget";
        case ArtifactType::Prefab: return "prefab";
        case ArtifactType::Scene: return "scene";
        case ArtifactType::Shader: return "shader";
        case ArtifactType::Audio: return "audio";
        case ArtifactType::Unknown:
        case ArtifactType::Count:
        default:
            return {};
        }
    }

    bool IsContentHashBoundToArtifactPath(
        const std::string& artifactPath,
        const std::string& contentHash)
    {
        if (contentHash.rfind("sha256:", 0u) != 0u)
            return false;

        const auto fileName = std::filesystem::path(artifactPath).filename().generic_string();
        return NLS::Core::Assets::IsArtifactStorageFileName(fileName) &&
            contentHash == "sha256:" + fileName;
    }

    bool IsValidRuntimeArtifactEntry(
        const NLS::Core::Assets::ArtifactType artifactType,
        const std::string& loaderId,
        const std::string& artifactPath,
        const std::string& contentHash)
    {
        const auto expectedLoaderId = LoaderIdForArtifactType(artifactType);
        return !expectedLoaderId.empty() &&
            loaderId == expectedLoaderId &&
            !artifactPath.empty() &&
            IsContentHashBoundToArtifactPath(artifactPath, contentHash) &&
            NLS::Core::Assets::IsContentStorageArtifactPath(artifactPath);
    }

    void RegisterRuntimeManifestArtifactAuthorizations(
        const NLS::Engine::Assets::RuntimeAssetManifest& manifest)
    {
        NLS::Core::Assets::ClearRuntimeArtifactAuthorization();
        for (const auto& entry : manifest.entries)
            NLS::Core::Assets::RegisterRuntimeAuthorizedArtifactPath(entry.artifactPath);
        for (const auto& pack : manifest.assetPacks)
        {
            for (const auto& entry : pack.entries)
                NLS::Core::Assets::RegisterRuntimeAuthorizedArtifactPath(entry.artifactPath);
        }
        NLS::Core::Assets::SetRuntimeArtifactAuthorizationEnabled(true);
    }

    bool IsMaterialEntry(
        const NLS::Core::Assets::ArtifactType artifactType,
        const std::string& loaderId)
    {
        return artifactType == NLS::Core::Assets::ArtifactType::Material || loaderId == "material";
    }

    bool IsRuntimeDependencyKind(const NLS::Core::Assets::AssetDependencyKind kind)
    {
        return kind == NLS::Core::Assets::AssetDependencyKind::ImportedArtifact ||
            kind == NLS::Core::Assets::AssetDependencyKind::PrefabBase ||
            kind == NLS::Core::Assets::AssetDependencyKind::NestedPrefab;
    }

    std::optional<NLS::Engine::Assets::RuntimeAssetRef> ToRuntimeDependencyRef(
        const NLS::Core::Assets::AssetDependencyRecord& dependency)
    {
        if (!IsRuntimeDependencyKind(dependency.kind))
            return std::nullopt;

        const auto parsedGuid = NLS::Guid::TryParse(dependency.value);
        if (!parsedGuid.has_value() || dependency.hashOrVersion.empty())
            return std::nullopt;

        return NLS::Engine::Assets::RuntimeAssetRef {
            NLS::Core::Assets::AssetId(*parsedGuid),
            dependency.hashOrVersion
        };
    }

    std::vector<NLS::Engine::Assets::RuntimeAssetRef> BuildRuntimeDependencies(
        const NLS::Core::Assets::ArtifactManifest& manifest,
        const NLS::Core::Assets::ArtifactDatabaseRecord& record)
    {
        std::vector<NLS::Engine::Assets::RuntimeAssetRef> dependencies;
        for (const auto& dependency : manifest.dependencies)
        {
            auto reference = ToRuntimeDependencyRef(dependency);
            if (reference.has_value() &&
                std::find(dependencies.begin(), dependencies.end(), *reference) == dependencies.end())
            {
                dependencies.push_back(*reference);
            }
        }
        // Imported model roots own the embedded resources in the same manifest.
        if (record.subAssetKey == manifest.primarySubAssetKey ||
            record.artifactType == NLS::Core::Assets::ArtifactType::Prefab ||
            record.artifactType == NLS::Core::Assets::ArtifactType::Model)
        {
            for (const auto& artifact : manifest.subAssets)
            {
                if (artifact.subAssetKey == record.subAssetKey)
                    continue;
                const NLS::Engine::Assets::RuntimeAssetRef reference {
                    artifact.sourceAssetId.IsValid() ? artifact.sourceAssetId : manifest.sourceAssetId,
                    artifact.subAssetKey
                };
                if (std::find(dependencies.begin(), dependencies.end(), reference) == dependencies.end())
                    dependencies.push_back(reference);
            }
        }
        return dependencies;
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

    NLS::Engine::Assets::RuntimeAssetManifestEntry MakeRuntimeEntry(
        const NLS::Core::Assets::ArtifactDatabaseRecord& record,
        std::vector<NLS::Engine::Assets::RuntimeAssetRef> dependencies)
    {
        return {
            record.artifactSourceAssetId.IsValid() ? record.artifactSourceAssetId : record.sourceAssetId,
            record.subAssetKey,
            record.artifactType,
            record.loaderId,
            record.artifactPath,
            record.contentHash,
            std::move(dependencies)
        };
    }
}

std::filesystem::path ResolveRuntimeArtifactDatabasePathForProjectSettings(
    const std::filesystem::path& projectSettingsPath)
{
    if (!projectSettingsPath.empty())
    {
        const auto projectRoot = projectSettingsPath.parent_path();
        if (!projectRoot.empty())
            return projectRoot / "Library" / "ArtifactDB";
    }

    return std::filesystem::current_path() / "Data" / "ArtifactDB";
}

std::optional<PackagedRuntime> DetectPackagedRuntime(const std::filesystem::path& installRoot)
{
    if (installRoot.empty())
        return std::nullopt;

    std::error_code error;
    const auto contentRoot = installRoot / "Data" / "User";
    if (!std::filesystem::is_directory(contentRoot, error))
        return std::nullopt;
    if (!std::filesystem::is_regular_file(contentRoot / "Game.ini", error))
        return std::nullopt;

    std::string descriptorError;
    const auto descriptor = NLS::Engine::Assets::PlayerDescriptor::LoadForInstallRoot(installRoot, &descriptorError);
    if (!descriptor.has_value())
        return std::nullopt;

    PackagedRuntime runtime;
    runtime.installRoot = installRoot;
    runtime.contentRoot = contentRoot;
    runtime.packagesRoot = contentRoot / "Packages";
    runtime.descriptor = *descriptor;
    runtime.manifestPath = descriptor->runtimeManifestPath.empty()
        ? contentRoot / "RuntimeAssetManifest.json"
        : installRoot / std::filesystem::path(descriptor->runtimeManifestPath);

    for (const auto& package : descriptor->contentPackages)
    {
        const auto packagePath = installRoot / std::filesystem::path(package);
        if (!std::filesystem::is_regular_file(packagePath, error))
        {
            NLS_LOG_ERROR("Packaged runtime is missing content package: " + packagePath.string());
            return std::nullopt;
        }
        runtime.packages.push_back(packagePath);
    }
    return runtime;
}

bool ValidatePackagedRuntimeCompatibility(const PackagedRuntime& runtime, std::string* error)
{
    const auto fail = [error](const std::string& message)
    {
        if (error != nullptr)
            *error = message;
        return false;
    };

    if (runtime.descriptor.schemaVersion == 0u || runtime.descriptor.schemaVersion > 1u)
        return fail("Player.info has an unsupported schema version");

    // Mount the shipped packages before validating the manifest so artifact
    // existence checks read the same bytes the runtime will load.
    NLS::Core::Assets::ClearContentMounts();
    for (const auto& package : runtime.packages)
    {
        std::string mountError;
        auto archive = NLS::Core::Assets::NpakArchive::Open(package, &mountError);
        if (!archive.has_value())
            return fail(mountError);
        if (archive->GetInfo().formatVersion != NLS::Core::Assets::NpakFormatVersion)
        {
            return fail(
                "content package '" + package.string() + "' uses container version " +
                std::to_string(archive->GetInfo().formatVersion) + " but this player expects " +
                std::to_string(NLS::Core::Assets::NpakFormatVersion));
        }
        if (!NLS::Core::Assets::MountContentPackage(package, runtime.contentRoot, &mountError))
            return fail(mountError);
    }

    if (!runtime.descriptor.engineVersion.empty() &&
        runtime.descriptor.engineVersion != NLS::Core::kEngineVersionString)
    {
        return fail(
            "player engine version '" + runtime.descriptor.engineVersion +
            "' does not match runtime '" + NLS::Core::kEngineVersionString + "'");
    }

    const auto hostPlatform =
#if defined(_WIN32)
        std::string("windows");
#elif defined(__APPLE__)
        std::string("macos");
#else
        std::string("linux");
#endif
    const auto hostArchitecture =
#if defined(_M_ARM64) || defined(__aarch64__)
        std::string("arm64");
#else
        std::string("x64");
#endif
    if (!runtime.descriptor.targetId.empty() &&
        runtime.descriptor.targetId.rfind(hostPlatform + "-" + hostArchitecture + "-", 0u) != 0u)
    {
        return fail(
            "player target '" + runtime.descriptor.targetId +
            "' is not compatible with this " + hostPlatform + "-" + hostArchitecture + " runtime");
    }

    const auto expectedPlatform =
#if defined(_WIN32)
        std::string("win64");
#elif defined(__APPLE__)
        std::string("macos");
#else
        std::string("linux");
#endif
    const auto isArtifactPlatformCompatible = [](const std::string& artifactPlatform)
    {
#if defined(_WIN32)
        // Windows artifacts carry the graphics backend in their target id,
        // for example win64-dx12 or win64-vulkan. A bare win64 remains valid
        // for legacy packages.
        return artifactPlatform == "win64" ||
            artifactPlatform.rfind("win64-", 0u) == 0u;
#elif defined(__APPLE__)
        return artifactPlatform == "macos";
#else
        return artifactPlatform == "linux";
#endif
    };
    if (!runtime.descriptor.artifactPlatform.empty() &&
        !isArtifactPlatformCompatible(runtime.descriptor.artifactPlatform) &&
        !(runtime.descriptor.buildMode != "Shipping" && runtime.descriptor.artifactPlatform == "editor"))
    {
        return fail(
            "player content target '" + runtime.descriptor.artifactPlatform +
            "' does not match this " + expectedPlatform + " runtime");
    }
    return true;
}

std::optional<NLS::Engine::Assets::RuntimeAssetDatabase> LoadRuntimeAssetDatabaseForProjectSettings(
    const std::filesystem::path& projectSettingsPath,
    const PackagedRuntime* packagedRuntime)
{
    NLS::Core::Assets::ClearRuntimeArtifactAuthorization();
    NLS::Core::Assets::SetRuntimeArtifactAuthorizationEnabled(false);

    if (packagedRuntime != nullptr)
    {
        // Packaged installs read the manifest written by the build pipeline.
        std::ifstream manifestStream(packagedRuntime->manifestPath, std::ios::binary);
        if (!manifestStream)
        {
            NLS_LOG_ERROR("Packaged runtime is missing the runtime asset manifest: " +
                packagedRuntime->manifestPath.string());
            return std::nullopt;
        }
        std::stringstream manifestText;
        manifestText << manifestStream.rdbuf();

        std::string manifestError;
        auto manifest = NLS::Engine::Assets::DeserializeRuntimeAssetManifest(
            manifestText.str(),
            &manifestError);
        if (!manifest.has_value())
        {
            NLS_LOG_ERROR("Packaged runtime asset manifest is invalid: " + manifestError);
            return std::nullopt;
        }

        if (!packagedRuntime->descriptor.artifactPlatform.empty() &&
            manifest->targetPlatform != packagedRuntime->descriptor.artifactPlatform &&
            !(packagedRuntime->descriptor.buildMode != "Shipping" &&
                packagedRuntime->descriptor.artifactPlatform == "editor"))
        {
            NLS_LOG_ERROR(
                "Packaged runtime manifest targets '" + manifest->targetPlatform +
                "' but Player.info declares '" + packagedRuntime->descriptor.artifactPlatform + "'.");
            return std::nullopt;
        }

        RegisterRuntimeManifestArtifactAuthorizations(*manifest);
        return NLS::Engine::Assets::RuntimeAssetDatabase(std::move(*manifest));
    }

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    if (!artifactDatabase.Load(ResolveRuntimeArtifactDatabasePathForProjectSettings(projectSettingsPath)))
        return std::nullopt;

    NLS::Engine::Assets::RuntimeAssetManifest manifest;
    manifest.schemaVersion = 1u;

    const auto stats = artifactDatabase.GetStats();
    if (stats.totalRecords == 0u)
        return std::nullopt;

    // The development runtime accepts artifacts cooked for the host platform.
    // The value comes from the project settings the editor writes, not a
    // hard-coded win64 tag, so Linux/macOS development runs resolve too.
    std::string configuredPlatform;
    {
        std::ifstream settingsStream(projectSettingsPath, std::ios::binary);
        std::stringstream settingsText;
        if (settingsStream)
        {
            settingsText << settingsStream.rdbuf();
            std::stringstream lines(settingsText.str());
            std::string line;
            while (std::getline(lines, line))
            {
                if (line.rfind("runtime_target_platform=", 0u) == 0u)
                {
                    configuredPlatform = line.substr(std::string("runtime_target_platform=").size());
                    break;
                }
            }
        }
    }
    if (configuredPlatform.empty())
        configuredPlatform = NLS::Platform::Process::CurrentArtifactPlatformTag();

    std::vector<const NLS::Core::Assets::ArtifactDatabaseRecord*> records;
    records.reserve(stats.totalRecords);
    bool hasInvalidRecord = false;
    std::string targetPlatform;
    artifactDatabase.VisitRecords([&](const NLS::Core::Assets::ArtifactDatabaseRecord& record)
    {
        if (record.status != NLS::Core::Assets::ArtifactRecordStatus::UpToDate)
            return;
        if (record.targetPlatform.empty())
        {
            hasInvalidRecord = true;
            return;
        }
        if (record.targetPlatform != configuredPlatform)
            return;
        if (targetPlatform.empty())
            targetPlatform = record.targetPlatform;
        else if (targetPlatform != record.targetPlatform)
        {
            hasInvalidRecord = true;
            return;
        }
        if (!IsValidRuntimeArtifactEntry(
            record.artifactType,
            record.loaderId,
            record.artifactPath,
            record.contentHash))
        {
            hasInvalidRecord = true;
            return;
        }
        records.push_back(&record);
    });

    if (hasInvalidRecord || records.empty())
        return std::nullopt;

    for (const auto* record : records)
    {
        if (record == nullptr)
            return std::nullopt;

        if (manifest.targetPlatform.empty())
            manifest.targetPlatform = record->targetPlatform;
        const auto sourceManifest = artifactDatabase.BuildManifestForSource(record->sourceAssetId, record->targetPlatform);
        if (!sourceManifest.has_value())
            return std::nullopt;

        manifest.entries.push_back(MakeRuntimeEntry(*record, BuildRuntimeDependencies(*sourceManifest, *record)));
        if (record->primarySubAssetKey == record->subAssetKey)
        {
            AddUniqueReference(manifest.roots, {
                record->artifactSourceAssetId.IsValid() ? record->artifactSourceAssetId : record->sourceAssetId,
                record->subAssetKey
            });
        }
    }

    RegisterRuntimeManifestArtifactAuthorizations(manifest);
    return NLS::Engine::Assets::RuntimeAssetDatabase(std::move(manifest));
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

        // A packaged Player has a runtime manifest, not the editor ArtifactDB.
        // Resolve the material's frozen shader dependencies through that same
        // manifest and register their native artifacts before material creation.
        if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ShaderManager>())
        {
            using namespace NLS::Render::Resources::Loaders;
            const auto payload = MaterialLoader::ReadSerializedPayload(
                NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(entry->artifactPath));
            std::istringstream lines(payload);std::string line, shaderSource;
            while (std::getline(lines, line))
                if (line.starts_with("shader="))
                {shaderSource=line.substr(7);if(!shaderSource.empty()&&shaderSource.back()=='\r')shaderSource.pop_back();break;}
            if (!shaderSource.empty())
            {
                std::queue<NLS::Engine::Assets::RuntimeAssetRef> shaderPending;
                std::unordered_set<NLS::Engine::Assets::RuntimeAssetRef> shaderVisited;
                for (const auto& dependency : entry->dependencies) shaderPending.push(dependency);
                std::vector<std::string> artifacts;
                while (!shaderPending.empty())
                {
                    const auto dependency=shaderPending.front();shaderPending.pop();
                    if(!shaderVisited.insert(dependency).second)continue;
                    const auto* shaderEntry=runtimeDatabase.Resolve(dependency);if(!shaderEntry)continue;
                    for(const auto& child:shaderEntry->dependencies)shaderPending.push(child);
                    if(shaderEntry->artifactType!=NLS::Core::Assets::ArtifactType::Shader)continue;
                    if(NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ShaderManager>().GetResource(shaderEntry->artifactPath,true))
                        artifacts.push_back(shaderEntry->artifactPath);
                }
                RegisterShaderPassArtifacts(shaderSource,std::move(artifacts));
            }
        }

        if (materialManager.LoadArtifactWithoutTextures(
            entry->artifactPath, runtimeDatabase.GetManifest().targetPlatform) != nullptr)
            ++loadedCount;
    }

    return loadedCount;
}
}
