#include "Core/PrefabInstanceResourceLifetime.h"

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Rendering/Resources/Material.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace NLS::Editor::Core
{
    namespace
    {
        std::optional<NLS::Core::ResourceManagement::ResourceLifetimeResourceType>
        ResourceLifetimeTypeFromPrefabResolvedAsset(
            const NLS::Engine::Assets::PrefabResolvedAsset& resolved)
        {
            using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;
            if (resolved.expectedType == "Mesh")
                return ResourceLifetimeResourceType::Mesh;
            if (resolved.expectedType == "Material")
                return ResourceLifetimeResourceType::Material;
            if (resolved.expectedType == "Texture")
                return ResourceLifetimeResourceType::Texture;
            return std::nullopt;
        }

        std::string LowercaseExtension(const std::string& path)
        {
            auto extension = std::filesystem::path(path).extension().string();
            std::transform(
                extension.begin(),
                extension.end(),
                extension.begin(),
                [](const unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
            return extension;
        }

        bool ResolvedAssetPathMatchesType(
            const NLS::Core::ResourceManagement::ResourceLifetimeResourceType type,
            const std::string& path)
        {
            if (path.empty())
                return false;

            const auto extension = LowercaseExtension(path);
            using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;
            switch (type)
            {
            case ResourceLifetimeResourceType::Mesh:
                return extension == ".nmesh";
            case ResourceLifetimeResourceType::Material:
                return extension == ".nmat";
            case ResourceLifetimeResourceType::Texture:
                return extension == ".ntex";
            default:
                return false;
            }
        }

        struct ResourceOwnerPath
        {
            NLS::Core::ResourceManagement::ResourceLifetimeResourceType type =
                NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh;
            std::string path;
        };

        struct LiveRendererArtifactOwnerScan
        {
            std::vector<ResourceOwnerPath> paths;
            std::vector<ResourceOwnerPath> unresolvedExpectedPaths;
        };

        template<typename Callback>
        void ForEachGameObjectInSubtree(NLS::Engine::GameObject& root, const Callback& callback)
        {
            std::vector<NLS::Engine::GameObject*> stack { &root };
            while (!stack.empty())
            {
                auto* object = stack.back();
                stack.pop_back();
                if (!object || !object->IsAlive())
                    continue;

                callback(*object);
                for (auto* child : object->GetChildren())
                {
                    if (child)
                        stack.push_back(child);
                }
            }
        }

        LiveRendererArtifactOwnerScan ScanLiveRendererArtifactResourceOwnerPaths(
            NLS::Engine::GameObject& root)
        {
            using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;
            LiveRendererArtifactOwnerScan scan;
            ForEachGameObjectInSubtree(
                root,
                [&scan](NLS::Engine::GameObject& object)
                {
                    if (auto* meshFilter = object.GetComponent<NLS::Engine::Components::MeshFilter>())
                    {
                        const auto meshPath = meshFilter->GetModelPath();
                        if (ResolvedAssetPathMatchesType(ResourceLifetimeResourceType::Mesh, meshPath))
                            scan.paths.push_back({ ResourceLifetimeResourceType::Mesh, meshPath });
                        else if (!meshPath.empty())
                        {
                            scan.unresolvedExpectedPaths.push_back({
                                ResourceLifetimeResourceType::Mesh,
                                meshPath });
                        }
                    }

                    auto* meshRenderer = object.GetComponent<NLS::Engine::Components::MeshRenderer>();
                    if (meshRenderer == nullptr)
                        return;

                    const auto materialPaths = meshRenderer->GetMaterialPaths();
                    for (size_t index = 0u;
                        index < NLS::Engine::Components::MeshRenderer::kMaxMaterialCount;
                        ++index)
                    {
                        auto* material = meshRenderer->GetMaterialAtIndex(static_cast<uint8_t>(index));
                        const auto materialPath = index < materialPaths.size()
                            ? materialPaths[index]
                            : std::string {};
                        const auto resolvedMaterialPath = !materialPath.empty()
                            ? materialPath
                            : (material != nullptr ? material->path : std::string {});
                        if (ResolvedAssetPathMatchesType(
                                ResourceLifetimeResourceType::Material,
                                resolvedMaterialPath))
                        {
                            scan.paths.push_back({
                                ResourceLifetimeResourceType::Material,
                                resolvedMaterialPath });
                        }
                        else if (!resolvedMaterialPath.empty())
                        {
                            scan.unresolvedExpectedPaths.push_back({
                                ResourceLifetimeResourceType::Material,
                                resolvedMaterialPath });
                        }

                        if (material == nullptr || !material->IsValid())
                            continue;

                        for (const auto& [_, texturePath] : material->GetTextureResourcePaths())
                        {
                            if (ResolvedAssetPathMatchesType(
                                    ResourceLifetimeResourceType::Texture,
                                    texturePath))
                            {
                                scan.paths.push_back({
                                    ResourceLifetimeResourceType::Texture,
                                    texturePath });
                            }
                            else if (!texturePath.empty())
                            {
                                scan.unresolvedExpectedPaths.push_back({
                                    ResourceLifetimeResourceType::Texture,
                                    texturePath });
                            }
                        }
                    }
                });

            return scan;
        }

        bool PreservedResolvedAssetCoversUnresolvedLiveRendererPath(
            const NLS::Engine::Assets::PrefabResolvedAsset& resolved,
            const ResourceOwnerPath& unresolved)
        {
            const auto type = ResourceLifetimeTypeFromPrefabResolvedAsset(resolved);
            if (!type.has_value() || *type != unresolved.type)
                return false;
            if (!ResolvedAssetPathMatchesType(*type, resolved.artifactPath))
                return false;
            if (resolved.subAssetKey == unresolved.path)
                return true;

            const auto normalizedArtifactPath =
                NLS::Core::ResourceManagement::ResourceLifetimeRegistry::NormalizeResourcePath(
                    resolved.artifactPath);
            const auto normalizedUnresolvedPath =
                NLS::Core::ResourceManagement::ResourceLifetimeRegistry::NormalizeResourcePath(
                    unresolved.path);
            return normalizedArtifactPath == normalizedUnresolvedPath;
        }

        bool PreservedResolvedAssetsCoverUnresolvedLiveRendererPaths(
            const std::vector<NLS::Engine::Assets::PrefabResolvedAsset>& preservedResolvedAssets,
            const LiveRendererArtifactOwnerScan& liveScan)
        {
            for (const auto& unresolved : liveScan.unresolvedExpectedPaths)
            {
                const auto covered = std::any_of(
                    preservedResolvedAssets.begin(),
                    preservedResolvedAssets.end(),
                    [&unresolved](const NLS::Engine::Assets::PrefabResolvedAsset& resolved)
                    {
                        return PreservedResolvedAssetCoversUnresolvedLiveRendererPath(
                            resolved,
                            unresolved);
                    });
                if (!covered)
                    return false;
            }
            return true;
        }

        std::vector<ResourceOwnerPath> CollectPreservedArtifactResourceOwnerPaths(
            const std::vector<NLS::Engine::Assets::PrefabResolvedAsset>& resolvedAssets)
        {
            std::vector<ResourceOwnerPath> paths;
            std::unordered_set<std::string> seen;
            for (const auto& resolved : resolvedAssets)
            {
                const auto type = ResourceLifetimeTypeFromPrefabResolvedAsset(resolved);
                if (!type.has_value())
                    continue;
                if (!ResolvedAssetPathMatchesType(*type, resolved.artifactPath))
                    continue;

                const auto key =
                    std::to_string(static_cast<uint8_t>(*type)) +
                    "\n" +
                    NLS::Core::ResourceManagement::ResourceLifetimeRegistry::NormalizeResourcePath(
                        resolved.artifactPath);
                if (!seen.insert(key).second)
                    continue;

                paths.push_back({ *type, resolved.artifactPath });
            }
            return paths;
        }

        void AppendUniqueResourceOwnerPaths(
            std::vector<ResourceOwnerPath>& target,
            const std::vector<ResourceOwnerPath>& source)
        {
            std::unordered_set<std::string> existing;
            for (const auto& path : target)
            {
                existing.insert(
                    std::to_string(static_cast<uint8_t>(path.type)) +
                    "\n" +
                    NLS::Core::ResourceManagement::ResourceLifetimeRegistry::NormalizeResourcePath(path.path));
            }

            for (const auto& path : source)
            {
                const auto key =
                    std::to_string(static_cast<uint8_t>(path.type)) +
                    "\n" +
                    NLS::Core::ResourceManagement::ResourceLifetimeRegistry::NormalizeResourcePath(path.path);
                if (existing.insert(key).second)
                    target.push_back(path);
            }
        }

        bool RebuildPrefabInstanceLiveRendererResourceOwnersForTrim(
            NLS::Core::ResourceManagement::ResourceLifetimeRegistry& registry,
            const std::string& ownerToken,
            const std::vector<ResourceOwnerPath>& paths)
        {
            if (paths.empty())
                return false;

            registry.ReleaseOwner(ownerToken);
            for (const auto& path : paths)
            {
                registry.Acquire({
                    ownerToken,
                    path.type,
                    path.path,
                    0u,
                    NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::SceneInstance });
            }
            return true;
        }
    }

    std::string BuildPrefabInstanceResourceOwnerToken(
        const NLS::Editor::Assets::PrefabInstanceRecord& instance)
    {
        if (instance.instanceRoot == nullptr)
            return {};

        return "scene-prefab:" + std::to_string(instance.instanceRoot->GetInstanceID());
    }

    bool ReleasePrefabInstanceResourceOwners(
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& registry,
        const std::string& ownerToken)
    {
        if (ownerToken.empty())
            return false;

        registry.ReleaseOwner(ownerToken);
        return true;
    }

    bool AcquirePrefabResolvedAssetResourceOwners(
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& registry,
        const std::string& ownerToken,
        const std::vector<NLS::Engine::Assets::PrefabResolvedAsset>& resolvedAssets)
    {
        if (ownerToken.empty())
            return false;

        bool acquired = false;
        std::unordered_set<std::string> seen;
        for (const auto& resolved : resolvedAssets)
        {
            const auto type = ResourceLifetimeTypeFromPrefabResolvedAsset(resolved);
            if (!type.has_value())
                continue;
            if (!ResolvedAssetPathMatchesType(*type, resolved.artifactPath))
                continue;
            const auto key =
                std::to_string(static_cast<uint8_t>(*type)) +
                "\n" +
                NLS::Core::ResourceManagement::ResourceLifetimeRegistry::NormalizeResourcePath(
                    resolved.artifactPath);
            if (!seen.insert(key).second)
                continue;

            registry.Acquire({
                ownerToken,
                *type,
                resolved.artifactPath,
                0u,
                NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::SceneInstance});
            acquired = true;
        }
        return acquired;
    }

    bool HasPrefabResolvedArtifactResourceOwners(
        const std::vector<NLS::Engine::Assets::PrefabResolvedAsset>& resolvedAssets)
    {
        for (const auto& resolved : resolvedAssets)
        {
            const auto type = ResourceLifetimeTypeFromPrefabResolvedAsset(resolved);
            if (type.has_value() && ResolvedAssetPathMatchesType(*type, resolved.artifactPath))
                return true;
        }
        return false;
    }

    bool RebuildPrefabInstancePreservedResourceOwnersForTrim(
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& registry,
        const std::string& ownerToken,
        const NLS::Editor::Assets::PrefabInstanceRecord& instance)
    {
        if (!HasPrefabResolvedArtifactResourceOwners(instance.preservedResolvedAssets))
            return false;

        ReleasePrefabInstanceResourceOwners(registry, ownerToken);
        return AcquirePrefabResolvedAssetResourceOwners(
            registry,
            ownerToken,
            instance.preservedResolvedAssets);
    }

    PrefabInstanceResourceOwnerRefreshResult RefreshPrefabInstanceResourceOwnersForTrim(
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& lifetimeRegistry,
        const NLS::Editor::Assets::PrefabInstanceRegistry& registry)
    {
        PrefabInstanceResourceOwnerRefreshResult result;
        for (const auto& instance : registry.GetInstances())
        {
            auto* root = instance.instanceRoot;
            if (root == nullptr || !root->IsAlive())
                continue;

            const auto ownerToken = BuildPrefabInstanceResourceOwnerToken(instance);
            if (ownerToken.empty())
                continue;

            auto ownerPaths = CollectPreservedArtifactResourceOwnerPaths(instance.preservedResolvedAssets);
            const auto liveScan = ScanLiveRendererArtifactResourceOwnerPaths(*root);
            if (instance.generatedReadOnly &&
                !PreservedResolvedAssetsCoverUnresolvedLiveRendererPaths(
                    instance.preservedResolvedAssets,
                    liveScan))
            {
                result.hasDeferredGeneratedInstances = true;
                continue;
            }

            AppendUniqueResourceOwnerPaths(ownerPaths, liveScan.paths);
            const bool rebuilt = RebuildPrefabInstanceLiveRendererResourceOwnersForTrim(
                lifetimeRegistry,
                ownerToken,
                ownerPaths);
            if (rebuilt)
                ++result.rebuiltOwnerCount;

            if (instance.generatedReadOnly &&
                !HasPrefabResolvedArtifactResourceOwners(instance.preservedResolvedAssets) &&
                liveScan.paths.empty())
            {
                result.hasDeferredGeneratedInstances = true;
            }
        }
        return result;
    }

    bool ShouldDeferImportedResourceTrimForPrefabInstances(
        const NLS::Editor::Assets::PrefabInstanceRegistry& registry)
    {
        for (const auto& instance : registry.GetInstances())
        {
            auto* root = instance.instanceRoot;
            if (!instance.generatedReadOnly || root == nullptr || !root->IsAlive())
                continue;

            const auto preservedPaths = CollectPreservedArtifactResourceOwnerPaths(instance.preservedResolvedAssets);
            const auto liveScan = ScanLiveRendererArtifactResourceOwnerPaths(*root);
            if (!PreservedResolvedAssetsCoverUnresolvedLiveRendererPaths(
                    instance.preservedResolvedAssets,
                    liveScan))
                return true;

            if (preservedPaths.empty() && liveScan.paths.empty())
            {
                return true;
            }
        }
        return false;
    }

    PrefabInstanceMarkedDestroyCleanupResult CleanupPrefabInstanceMarkedDestroy(
        NLS::Editor::Assets::PrefabInstanceRegistry& registry,
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& lifetimeRegistry,
        NLS::Engine::GameObject& object)
    {
        PrefabInstanceMarkedDestroyCleanupResult result;
        std::vector<NLS::Engine::GameObject*> rootInstancesToRemove;
        for (const auto& instance : registry.GetInstances())
        {
            auto* root = instance.instanceRoot;
            if (root == nullptr)
                continue;

            if (root == &object || root->IsDescendantOf(&object))
                rootInstancesToRemove.push_back(root);
        }

        for (auto* root : rootInstancesToRemove)
        {
            const auto* instance = registry.FindRootInstance(*root);
            if (instance == nullptr)
                continue;

            result.releasedOwnerToken = BuildPrefabInstanceResourceOwnerToken(*instance);
            result.removedRootInstance = true;
            ReleasePrefabInstanceResourceOwners(lifetimeRegistry, result.releasedOwnerToken);
            registry.RemoveRootInstance(*root);
        }

        if (result.removedRootInstance)
            return result;

        result.removedObjectMapping = registry.RemoveObjectMapping(object);
        return result;
    }
}
