#pragma once

#include "Assets/PrefabEditorWorkflow.h"
#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"
#include "GameObject.h"

#include <cstddef>
#include <string>
#include <vector>

namespace NLS::Editor::Core
{
    struct PrefabInstanceMarkedDestroyCleanupResult
    {
        bool removedRootInstance = false;
        bool removedObjectMapping = false;
        std::string releasedOwnerToken;
    };

    struct PrefabInstanceResourceOwnerRefreshResult
    {
        size_t rebuiltOwnerCount = 0u;
        bool hasDeferredGeneratedInstances = false;
    };

    std::string BuildPrefabInstanceResourceOwnerToken(
        const NLS::Editor::Assets::PrefabInstanceRecord& instance);

    bool ReleasePrefabInstanceResourceOwners(
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& registry,
        const std::string& ownerToken);

    bool AcquirePrefabResolvedAssetResourceOwners(
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& registry,
        const std::string& ownerToken,
        const std::vector<NLS::Engine::Assets::PrefabResolvedAsset>& resolvedAssets);

    bool HasPrefabResolvedArtifactResourceOwners(
        const std::vector<NLS::Engine::Assets::PrefabResolvedAsset>& resolvedAssets);

    bool RebuildPrefabInstancePreservedResourceOwnersForTrim(
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& registry,
        const std::string& ownerToken,
        const NLS::Editor::Assets::PrefabInstanceRecord& instance);

    PrefabInstanceResourceOwnerRefreshResult RefreshPrefabInstanceResourceOwnersForTrim(
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& lifetimeRegistry,
        const NLS::Editor::Assets::PrefabInstanceRegistry& registry);

    bool ShouldDeferImportedResourceTrimForPrefabInstances(
        const NLS::Editor::Assets::PrefabInstanceRegistry& registry);

    PrefabInstanceMarkedDestroyCleanupResult CleanupPrefabInstanceMarkedDestroy(
        NLS::Editor::Assets::PrefabInstanceRegistry& registry,
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& lifetimeRegistry,
        NLS::Engine::GameObject& object);
}
