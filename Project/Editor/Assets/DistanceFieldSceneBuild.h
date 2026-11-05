#pragma once

#include <string>
#include <vector>

namespace NLS::Engine::SceneSystem { class Scene; }
namespace NLS::Editor::Assets
{
class AssetDatabaseFacade;

// Resolve authoring identities, including meshes that have not finished loading.
// A source model is imported once even when its sections have many instances.
std::vector<std::string> CollectSceneDistanceFieldModelAssets(
    const Engine::SceneSystem::Scene& scene, const AssetDatabaseFacade& database);
}
