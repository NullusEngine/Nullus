#include <Engine/ECS/Baking.h>
#include <Engine/ECS/ECSAssets.h>
#include <Engine/ECS/RuntimeWorld.h>
#include <Engine/ECS/TransformHierarchy.h>
#include <Engine/GameObject.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Components/TransformComponent.h>
#include <Engine/Assets/RuntimeManifestSerialization.h>
#include <Engine/Assets/PlayerDescriptor.h>
#include <Core/Version.h>
#include <Cook/CookTypes.h>
#include <Build/BuildDependencyCollector.h>
#include <Core/Assets/AssetMeta.h>
#include <Core/Assets/ContentSource.h>
#include <Core/Assets/NpakArchive.h>
#include <Base/Jobs/JobSystem.h>
#include <fstream>
#include <iostream>

int main()
{
    using namespace NLS;
    using namespace NLS::ECS;
    using namespace NLS::Engine::ECSIntegration;
    namespace CA = NLS::Core::Assets;
    namespace EA = NLS::Engine::Assets;
    try
    {
        Base::Jobs::JobSystemConfig jobConfig; jobConfig.workerCount = 4;
        if (!Base::Jobs::TryInitializeJobSystem(jobConfig)) return 1;
        struct Jobs { ~Jobs() { Base::Jobs::ShutdownJobSystem(); } } jobs;
        const auto project = std::filesystem::absolute(std::filesystem::path("tmp") / ("ecs-asset-example-" + Guid::New().ToString()));
        const auto assets = project / "Assets"; std::filesystem::create_directories(assets);
        const auto scenePath = assets / "World.ecsscene", prefabPath = assets / "Prop.ecsprefab";
        const Engine::Serialize::ObjectId rootId{Guid::NewDeterministic("ECS.AssetExample.Root")};
        const Engine::Serialize::ObjectId childId{Guid::NewDeterministic("ECS.AssetExample.Child")};
        World schemas; RegisterTransformComponents(schemas); BakingPipeline baking(schemas.GetRegistryLifetime()); RegisterTransformBaker(baking);
        {
            Engine::SceneSystem::Scene authoring;
            auto& root = authoring.CreateGameObject("root"); auto& child = authoring.CreateGameObject("child");
            root.GetTransform()->SetLocalPosition({9, 0, 0}); child.SetParent(root); child.GetTransform()->SetLocalPosition({4, 0, 0});
            const std::array sources{BakingSource{rootId, root.GetInstanceID()}, BakingSource{childId, child.GetInstanceID()}};
            BakingOptions options; options.outputPath = scenePath; const auto result = baking.Bake(sources, options);
            if (!result.succeeded) throw std::runtime_error(result.diagnostics.at(0));
        } // Actual GameObjects are gone before any runtime loading.
        auto prefab = WorldArchive::ReadFile(scenePath); prefab.kind = DocumentKind::Prefab; WorldArchive::WriteFile(prefabPath, prefab);
        const auto sceneMeta = CA::AssetMeta::CreateForAsset(scenePath), prefabMeta = CA::AssetMeta::CreateForAsset(prefabPath);
        if (!sceneMeta.Save(CA::GetAssetMetaPath(scenePath)) || !prefabMeta.Save(CA::GetAssetMetaPath(prefabPath)))
            throw std::runtime_error("Cannot create native ECS asset metadata");
        Cook::CookOptions cook; cook.projectRoot = project; cook.assetsRoot = assets; cook.artifactDatabasePath = project / "Library/ArtifactDB";
        cook.target.artifactPlatform = "win64";
        const auto cooked = Cook::AssetCooker::CookAssets(cook, {scenePath, prefabPath});
        if (!cooked.Succeeded()) throw std::runtime_error(cooked.FormatSummary());
        CA::ArtifactDatabase database; if (!database.Load(cook.artifactDatabasePath)) throw std::runtime_error(database.GetLastError());
        EA::RuntimeManifestBuilder builder;
        for (const auto id : {sceneMeta.id, prefabMeta.id}) builder.AddArtifactManifest(database.BuildManifestForSource(id, "win64").value());
        auto built = builder.Build({{sceneMeta.id, "ecs-scene:main"}, {prefabMeta.id, "ecs-prefab:main"}}, "win64");
        if (built.manifest.entries.size() != 2) throw std::runtime_error("ECS artifact closure is incomplete");
        CA::NpakWriter writer;
        for (auto& entry : built.manifest.entries)
        {
            const auto host = Build::BuildDependencyCollector::ResolveArtifactHostPath(project, entry.artifactPath);
            auto portable = CA::TryMakePortableContentArtifactPath(entry.artifactPath);
            if (portable.starts_with("Library/")) portable.erase(0, 8);
            if (!writer.AddFileFromDisk(portable, host)) throw std::runtime_error("Cannot add ECS artifact to content package");
            entry.artifactPath = portable;
        }
        const auto packagePath = project / "base.npak", manifestPath = project / "runtime-assets.json";
        std::string error; if (!writer.Write(packagePath, {}, &error)) throw std::runtime_error(error);
        const auto manifestText = EA::SerializeRuntimeAssetManifest(built.manifest);
        { std::ofstream file(manifestPath, std::ios::binary); file << manifestText; if (!file) throw std::runtime_error("Cannot save runtime manifest"); }
        auto manifest = EA::DeserializeRuntimeAssetManifest(manifestText, &error); if (!manifest) throw std::runtime_error(error);
        auto opened = CA::NpakArchive::Open(packagePath, &error); if (!opened) throw std::runtime_error(error);
        auto package = std::make_shared<CA::NpakArchive>(std::move(*opened));
        auto source = std::make_shared<CA::PackageContentSource>(package); const auto dataRoot = project / "Data/User";
        std::filesystem::create_directories(dataRoot);
        { std::ofstream file(dataRoot / "RuntimeAssetManifest.json", std::ios::binary); file << manifestText; if (!file) throw std::runtime_error("Cannot save Player manifest"); }
        { std::ofstream file(dataRoot / "Game.ini", std::ios::binary); file << "start_scene=World.ecsscene\nstart_scene_asset=" << sceneMeta.id.ToString() << "\nstart_scene_subasset=ecs-scene:main\n"; if (!file) throw std::runtime_error("Cannot save Player settings"); }
        EA::PlayerDescriptor descriptor; descriptor.engineVersion = Core::kEngineVersionString;
        descriptor.artifactPlatform = "win64"; descriptor.buildMode = "Development"; descriptor.startScene = "World.ecsscene";
        descriptor.runtimeManifestPath = "Data/User/RuntimeAssetManifest.json"; descriptor.contentRoot = "Data/User"; descriptor.contentPackages = {"base.npak"};
        if (!descriptor.Save(project / EA::PlayerDescriptor::FileName(), &error)) throw std::runtime_error(error);
        CA::ContentSourceTable::Get().Mount(dataRoot.generic_string(), source);
        struct Mount
        {
            std::filesystem::path root; const CA::IContentSource* source;
            ~Mount() { CA::ContentSourceTable::Get().Unmount(root, source); }
        } mount{dataRoot, source.get()};
        std::filesystem::remove_all(project / "Library"); // This uniquely-created demo project owns the cache.
        RuntimeWorld runtime; RegisterTransformSystem(runtime.GetWorld());
        runtime.ConfigureAssets(std::make_shared<EA::RuntimeAssetDatabase>(std::move(*manifest)), dataRoot);
        const auto request = runtime.RequestSceneAsset(1, {sceneMeta.id, "ecs-scene:main"}, {.sceneOwner = 17});
        const auto loaded = runtime.Streaming().Wait(request);
        if (loaded.status != SceneLoadStatus::Loaded) throw std::runtime_error(loaded.error);
        const auto first = runtime.InstantiatePrefabAsset({prefabMeta.id, "ecs-prefab:main"});
        const auto second = runtime.InstantiatePrefabAsset({prefabMeta.id, "ecs-prefab:main"});
        runtime.RunPhase(SystemPhase::LateUpdate, {});
        const auto child = runtime.Streaming().Find(1, childId.GetGuid().ToString());
        const auto x = runtime.GetWorld().ReadComponent<WorldTransform>(child).matrix[3];
        std::cout << "baked_entities=2 cooked_assets=" << cooked.cooked << " packaged_artifacts=" << writer.GetAddedFileCount()
            << " scene_entities=" << loaded.entityCount << " total_entities=" << runtime.GetWorld().GetEntityCount()
            << " child_world_x=" << x << " background_parse=" << loaded.parsedOnBackgroundWorker << '\n';
        std::cout << "package=" << packagePath.generic_string() << " manifest=" << manifestPath.generic_string() << '\n';
        std::cout << "player_root=" << project.generic_string() << '\n';
        return runtime.GetWorld().GetEntityCount() == 6 && x == 13 && first.Find(rootId.GetGuid().ToString()) != second.Find(rootId.GetGuid().ToString()) ? 0 : 1;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
