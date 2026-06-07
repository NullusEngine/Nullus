#include "Core/PrefabInstanceResourceLifetime.h"

#include <vector>

namespace NLS::Editor::Core
{
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
