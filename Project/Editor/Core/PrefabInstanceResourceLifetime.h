#pragma once

#include "Assets/PrefabEditorWorkflow.h"
#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"
#include "GameObject.h"

#include <string>

namespace NLS::Editor::Core
{
    struct PrefabInstanceMarkedDestroyCleanupResult
    {
        bool removedRootInstance = false;
        bool removedObjectMapping = false;
        std::string releasedOwnerToken;
    };

    std::string BuildPrefabInstanceResourceOwnerToken(
        const NLS::Editor::Assets::PrefabInstanceRecord& instance);

    bool ReleasePrefabInstanceResourceOwners(
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& registry,
        const std::string& ownerToken);

    PrefabInstanceMarkedDestroyCleanupResult CleanupPrefabInstanceMarkedDestroy(
        NLS::Editor::Assets::PrefabInstanceRegistry& registry,
        NLS::Core::ResourceManagement::ResourceLifetimeRegistry& lifetimeRegistry,
        NLS::Engine::GameObject& object);
}
