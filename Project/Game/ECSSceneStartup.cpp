#include "ECSSceneStartup.h"
#include <Core/Assets/AssetMeta.h>
#include <Engine/ECS/ECSAssets.h>
#include <Engine/ECS/TransformHierarchy.h>
#include <stdexcept>

namespace NLS::Game::RuntimeAssets
{
bool IsECSStartScene(std::string_view path)
{ return NLS::Core::Assets::InferAssetType(std::filesystem::path(path)) == NLS::Core::Assets::AssetType::ECSScene; }

Engine::Assets::RuntimeAssetRef ResolveECSStartSceneAsset(std::string_view assetId,
    std::string_view subAssetKey, const std::filesystem::path& developmentSource)
{
    NLS::Core::Assets::AssetId id;
    if (!assetId.empty())
    {
        const auto guid = Guid::TryParse(std::string(assetId));
        if (!guid || !guid->IsValid()) throw std::invalid_argument("ECS start_scene_asset is not a valid GUID");
        id = NLS::Core::Assets::AssetId(*guid);
    }
    else if (!developmentSource.empty())
    {
        const auto meta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(developmentSource));
        if (meta && (meta->assetType == NLS::Core::Assets::AssetType::ECSScene ||
            (meta->assetType == NLS::Core::Assets::AssetType::Unknown && IsECSStartScene(developmentSource.generic_string())))) id = meta->id;
    }
    if (!id.IsValid()) throw std::invalid_argument("ECS startup requires start_scene_asset; rebuild the player or configure a valid ECS scene GUID");
    return {id, subAssetKey.empty() ? "ecs-scene:main" : std::string(subAssetKey)};
}

void ConfigureECSStartScene(Scripting::RuntimePlaySession& session,
    std::shared_ptr<const Engine::Assets::RuntimeAssetDatabase> database,
    std::filesystem::path contentRoot, Engine::Assets::RuntimeAssetRef scene,
    Scripting::RuntimePlaySession::ECSSetup registerGame)
{
    if (!database) throw std::invalid_argument("ECS startup requires a runtime asset database");
    const auto* entry = scene.subAssetKey.empty() ? database->ResolvePrimary(scene.assetId) : database->Resolve(scene);
    if (!entry || entry->artifactType != NLS::Core::Assets::ArtifactType::ECSScene || entry->loaderId != "ecs-scene")
        throw std::invalid_argument("ECS startup asset is absent or is not a cooked ECS scene");
    scene.subAssetKey = entry->subAssetKey;
    auto loader = std::make_shared<const Engine::ECSIntegration::ECSAssetLoader>(std::move(database), std::move(contentRoot));
    session.EnableECS([loader = std::move(loader), scene = std::move(scene), registerGame = std::move(registerGame)]
        (Engine::ECSIntegration::RuntimeWorld& runtime, Engine::SceneSystem::Scene& host)
    {
        Engine::ECSIntegration::RegisterTransformSystem(runtime.GetWorld());
        runtime.ConfigureAssets(loader);
        if (registerGame) registerGame(runtime, host);
        constexpr uint64_t startupChunk = 1;
        const auto loaded = runtime.Streaming().Wait(runtime.RequestSceneAsset(startupChunk, scene));
        if (loaded.status != ECS::SceneLoadStatus::Loaded)
            throw std::runtime_error("Cannot load ECS start scene: " + loaded.error);
    });
}
}
