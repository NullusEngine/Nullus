#include "Assets/DistanceFieldSceneBuild.h"

#include <set>
#include "Assets/AssetDatabaseFacade.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/SkinnedMeshRenderer.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include "Serialize/PPtr.h"

namespace NLS::Editor::Assets
{
std::vector<std::string> CollectSceneDistanceFieldModelAssets(
    const Engine::SceneSystem::Scene& scene, const AssetDatabaseFacade& database)
{
    std::set<std::string> paths;
    for (auto* object : scene.GetGameObjects())
    {
        if (!object || !object->IsActive() || object->IsEditorTransient()) continue;
        const auto* renderer = object->GetComponent<Engine::Components::MeshRenderer>();
        const auto* filter = object->GetComponent<Engine::Components::MeshFilter>();
        if (!renderer || !renderer->IsSelfEnabled() || !renderer->GetAffectDistanceField() || !filter ||
            object->GetComponent<Engine::Components::SkinnedMeshRenderer>()) continue;
        Engine::Serialize::ObjectIdentifier identifier;
        if (!Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
                filter->GetMeshReference().GetInstanceID(), identifier) || !identifier.IsAsset()) continue;
        auto path = database.GUIDToAssetPath(identifier.guid.ToString());
        if (path.starts_with("Assets/")) paths.insert(std::move(path));
    }
    return {paths.begin(), paths.end()};
}
}
