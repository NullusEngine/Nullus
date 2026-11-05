#include "JobTestSupport.h"
#include <ECSSceneStartup.h>
#include <RuntimeAssetManifestStartup.h>
#include <Engine/ECS/TransformHierarchy.h>
#include <Engine/ECS/ECSAssets.h>
#include <Core/Assets/AssetMeta.h>
#include <Core/Assets/ContentSource.h>
#include <Core/Filesystem/IniFile.h>
#include <Core/Version.h>
#include <Cook/CookTypes.h>
#include <Build/BuildDependencyCollector.h>
#include <Build/BuildPipelineStaging.h>
#include <Build/BuildReport.h>
#include <gtest/gtest.h>
#include <array>
#include <fstream>

namespace
{
using namespace NLS;
using namespace NLS::ECS;
using namespace NLS::Engine::ECSIntegration;
namespace CA = NLS::Core::Assets;
namespace Startup = NLS::Game::RuntimeAssets;
struct ResourceTag { std::string guid; };
void RegisterGame(World& world)
{
    world.RegisterComponent<ResourceTag>("startup.ResourceTag");
    RegisterComponentCodec<ResourceTag>(world.GetRegistry(),
        [](const ResourceTag& value, const EntityWriteContext& context) { return ComponentJson{{"guid", context.Resource(value.guid)}}; },
        [](ResourceTag& value, const ComponentJson& json, const EntityReadContext& context) { value.guid = context.Resource(json.at("guid").get<std::string>()); });
}
struct Project
{
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::path("tmp") / ("ecs-player-" + Guid::New().ToString()));
    std::filesystem::path assets = root / "Assets", databasePath = root / "Library/ArtifactDB", install = root / "Player";
    std::filesystem::path scenePath = assets / "World.ecsscene", prefabPath = assets / "Prop.ecsprefab";
    CA::AssetId sceneId, prefabId;
    Build::BuildContext context;
    Build::DependencyCollectionResult collection;
    Build::BuildReport report;
    std::vector<std::string> packages;
    Project()
    {
        std::filesystem::create_directories(assets);
        World world; RegisterTransformComponents(world);
        const auto parent = world.CreateEntity(), child = world.CreateEntity();
        AddTransform(world, parent, {{9, 0, 0}}); AddTransform(world, child, {{4, 0, 0}}, parent);
        auto doc = WorldArchive::CaptureAll(world); WorldArchive::WriteFile(scenePath, doc); sceneId = Meta(scenePath);
        doc.kind = DocumentKind::Prefab; WorldArchive::WriteFile(prefabPath, doc); prefabId = Meta(prefabPath);
        Write(root / "Game.ini", "start_scene=World.ecsscene\nruntime_target_platform=win64\nstart_scene_asset=stale-id\nstart_scene_subasset=old-key\n");
        context.projectRoot = root; context.projectFile = root / "Game.ini"; context.projectAssetsPath = assets;
        context.artifactDatabasePath = databasePath; context.libraryPath = root / "Library"; context.stagingRoot = install;
        context.profile.id = "ecs-player"; context.profile.productName = "ECS Startup"; context.profile.startScene = "World.ecsscene";
        context.profile.executableName = "Game"; context.profile.target.artifactPlatform = "win64";
        context.profile.target.scriptBackends = {Build::ScriptBackend::Lua};
        context.product.productName = "ECS Startup"; context.product.executableName = "Game";
        context.product.engineVersion = Core::kEngineVersionString;
    }
    ~Project()
    {
        CA::ClearContentMounts(); CA::ClearRuntimeArtifactAuthorization(); CA::SetRuntimeArtifactAuthorizationEnabled(false);
        std::error_code error; std::filesystem::remove_all(root, error);
    }
    static void Write(const std::filesystem::path& path, std::string_view text)
    { std::ofstream file(path, std::ios::binary); file.write(text.data(), static_cast<std::streamsize>(text.size())); }
    static CA::AssetId Meta(const std::filesystem::path& path)
    {
        auto meta = CA::AssetMeta::CreateForAsset(path);
        if (!meta.Save(CA::GetAssetMetaPath(path))) throw std::runtime_error("Cannot save fixture meta");
        return meta.id;
    }
    void AddDependency()
    {
        World world; RegisterTransformComponents(world); RegisterGame(world);
        const auto entity = world.CreateEntity(); AddTransform(world, entity, {{15, 0, 0}});
        world.AddComponent<ResourceTag>(entity, {prefabId.ToString()});
        WorldArchive::WriteFile(scenePath, WorldArchive::CaptureAll(world));
    }
    void Cook()
    {
        NLS::Cook::CookOptions options; options.projectRoot = root; options.assetsRoot = assets; options.artifactDatabasePath = databasePath;
        options.target.artifactPlatform = "win64";
        const auto summary = NLS::Cook::AssetCooker::CookAssets(options, {scenePath, prefabPath});
        if (!summary.Succeeded()) throw std::runtime_error("Fixture native Cook failed");
    }
    bool Collect()
    { return Build::BuildDependencyCollector::Collect(root, assets, context.profile, databasePath, collection, report); }
    void Stage()
    {
        if (!Collect()) throw std::runtime_error("Fixture collection failed");
        std::string error;
        if (!Build::Detail::StageContent(context, collection, report, packages, &error)) throw std::runtime_error(error);
        Build::PlayerTemplateInfo player; player.engineVersion = Core::kEngineVersionString;
        if (!Build::Detail::WritePlayerDescriptorFile(context, player, packages, report, &error)) throw std::runtime_error(error);
        if (!Build::Detail::VerifyStagedContent(context, report, packages, &error)) throw std::runtime_error(error);
    }
    std::shared_ptr<const Engine::Assets::RuntimeAssetDatabase> OpenPlayer()
    {
        const auto packaged = Startup::DetectPackagedRuntime(install);
        if (!packaged) throw std::runtime_error("Packaged runtime was not detected");
        std::string error;
        if (!Startup::ValidatePackagedRuntimeCompatibility(*packaged, &error)) throw std::runtime_error(error);
        auto database = Startup::LoadRuntimeAssetDatabaseForProjectSettings(install / "Data/User/Game.ini", &*packaged);
        if (!database) throw std::runtime_error("Player runtime manifest failed to load");
        return std::make_shared<const Engine::Assets::RuntimeAssetDatabase>(std::move(*database));
    }
};
TEST(ECSPlayer, NativeStartSceneIsACookedRootWithItsDependencyClosure)
{
    Project project; project.AddDependency(); project.Cook(); ASSERT_TRUE(project.Collect());
    ASSERT_EQ(project.collection.manifest.roots.size(), 1u); EXPECT_EQ(project.collection.manifest.roots[0].assetId, project.sceneId);
    EXPECT_EQ(project.collection.artifacts.size(), 2u);
    for (const auto& file : project.collection.rawFiles) EXPECT_NE(file.hostPath, project.scenePath);
}
TEST(ECSPlayer, AcceptsProjectMountPrefixAndRejectsMissingStartupMetadata)
{
    Project project; project.Cook(); project.context.profile.startScene = "Assets/World.ecsscene"; project.Stage();
    Filesystem::IniFile settings((project.install / "Data/User/Game.ini").string());
    EXPECT_EQ(settings.Get<std::string>("start_scene"), "World.ecsscene");
    std::filesystem::remove(CA::GetAssetMetaPath(project.scenePath)); project.report = {}; EXPECT_FALSE(project.Collect());
    EXPECT_TRUE(project.report.HasErrors());
}
TEST(ECSPlayer, StagingWritesStableIdentityAndShipsNoRequiredLooseECSScene)
{
    Project project; project.Cook(); project.Stage();
    Filesystem::IniFile settings((project.install / "Data/User/Game.ini").string());
    EXPECT_EQ(settings.Get<std::string>("start_scene_asset"), project.sceneId.ToString());
    EXPECT_EQ(settings.Get<std::string>("start_scene_subasset"), "ecs-scene:main");
    EXPECT_FALSE(std::filesystem::exists(project.install / "Data/User/Assets/World.ecsscene"));
    EXPECT_FALSE(project.packages.empty());
}
TEST(ECSPlayer, PackagedStartupLoadsBeforeBeginTicksAndRestartsWithoutDevelopmentFiles)
{
    ECSTest::JobSystemScope jobs; Project project; project.Cook(); project.Stage();
    std::filesystem::remove_all(project.assets); std::filesystem::remove_all(project.root / "Library");
    auto database = project.OpenPlayer(); Filesystem::IniFile settings((project.install / "Data/User/Game.ini").string());
    const auto asset = Startup::ResolveECSStartSceneAsset(settings.Get<std::string>("start_scene_asset"), settings.Get<std::string>("start_scene_subasset"));
    Scripting::ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    Engine::SceneSystem::SceneManager scenes; Scripting::RuntimePlaySession session;
    Startup::ConfigureECSStartScene(session, database, project.install / "Data/User", asset);
    ASSERT_TRUE(session.Begin(scripts, scenes, 4101).Succeeded()); ASSERT_NE(session.GetECS(), nullptr);
    auto& world = session.GetECS()->GetWorld(); EXPECT_EQ(world.GetEntityCount(), 2u);
    const auto old = session.GetECS()->Streaming().GetEntities(1).front();
    scenes.GetCurrentScene()->Play(); // The production Game host does this after Begin.
    session.Tick(0.02f); int children = 0;
    world.Query<Read<Parent>, Read<WorldTransform>>().Each([&](Entity, const Parent& parent, const WorldTransform& transform)
    { if (parent.entity.IsValid()) { ++children; EXPECT_FLOAT_EQ(transform.matrix[3], 13); } });
    EXPECT_EQ(children, 1); ASSERT_TRUE(session.End().Succeeded()); EXPECT_EQ(session.GetECS(), nullptr);
    ASSERT_TRUE(session.Begin(scripts, scenes, 4102).Succeeded()); EXPECT_EQ(session.GetECS()->GetWorld().GetEntityCount(), 2u);
    EXPECT_FALSE(session.GetECS()->GetWorld().IsAlive(old)); EXPECT_TRUE(session.End().Succeeded());
}
TEST(ECSPlayer, CustomCodecRegistrationRunsBeforeSceneDecodeAndMissingCodecFailsCleanly)
{
    ECSTest::JobSystemScope jobs; Project project; project.AddDependency(); project.Cook(); project.Stage(); auto database = project.OpenPlayer();
    Scripting::ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded()); Engine::SceneSystem::SceneManager scenes;
    Scripting::RuntimePlaySession session; const Engine::Assets::RuntimeAssetRef asset{project.sceneId, "ecs-scene:main"};
    Startup::ConfigureECSStartScene(session, database, project.install / "Data/User", asset);
    EXPECT_FALSE(session.Begin(scripts, scenes, 4201).Succeeded()); EXPECT_FALSE(session.IsActive()); EXPECT_EQ(session.GetECS(), nullptr);
    Startup::ConfigureECSStartScene(session, database, project.install / "Data/User", asset,
        [](RuntimeWorld& runtime, Engine::SceneSystem::Scene&) { RegisterGame(runtime.GetWorld()); });
    ASSERT_TRUE(session.Begin(scripts, scenes, 4202).Succeeded());
    EXPECT_EQ(session.GetECS()->GetWorld().Query<Read<ResourceTag>>().Count(), 1u); EXPECT_TRUE(session.End().Succeeded());
}
TEST(ECSPlayer, CorruptContainerAbortsBeginWithoutPublishingAWorld)
{
    ECSTest::JobSystemScope jobs; Project project; project.Cook(); ASSERT_TRUE(project.Collect());
    auto database = std::make_shared<const Engine::Assets::RuntimeAssetDatabase>(project.collection.manifest);
    ASSERT_FALSE(project.collection.artifacts.empty()); Project::Write(project.collection.artifacts.front().hostPath, "corrupt");
    Scripting::ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded()); Engine::SceneSystem::SceneManager scenes; Scripting::RuntimePlaySession session;
    Startup::ConfigureECSStartScene(session, database, project.root, {project.sceneId, "ecs-scene:main"});
    const auto status = session.Begin(scripts, scenes, 4301); EXPECT_FALSE(status.Succeeded()); EXPECT_FALSE(status.message.empty());
    EXPECT_FALSE(session.IsActive()); EXPECT_EQ(session.GetECS(), nullptr);
}
TEST(ECSPlayer, ExplicitIdentityIsRequiredForPackagesAndDevelopmentMetaIsOptIn)
{
    Project project; EXPECT_TRUE(Startup::IsECSStartScene("Assets/WORLD.ECSSCENE")); EXPECT_FALSE(Startup::IsECSStartScene("World.scene"));
    EXPECT_THROW(Startup::ResolveECSStartSceneAsset({}, {}), std::invalid_argument);
    EXPECT_THROW(Startup::ResolveECSStartSceneAsset("invalid-guid", {}, project.scenePath), std::invalid_argument);
    EXPECT_EQ(Startup::ResolveECSStartSceneAsset({}, {}, project.scenePath).assetId, project.sceneId);
    EXPECT_EQ(Startup::ResolveECSStartSceneAsset(project.sceneId.ToString(), {}).subAssetKey, "ecs-scene:main");
}
TEST(ECSPlayer, LegacyStartSceneKeepsRawGraphAndClearsObsoleteECSIdentity)
{
    Project project; project.Cook(); project.context.profile.startScene = "Assets/Main.scene";
    // The existing package writer requires a nonempty cooked payload. Keep a
    // real dynamic prefab while exercising the legacy raw startup document.
    project.context.profile.keepAssetPaths = {"Prop.ecsprefab"};
    Project::Write(project.assets / "Main.scene", ComponentJson{{"format", "Nullus.ObjectGraph.Scene"}, {"documentId", Guid::New().ToString()},
        {"root", Guid::New().ToString()}, {"objects", ComponentJson::array()}}.dump());
    project.Stage(); Filesystem::IniFile settings((project.install / "Data/User/Game.ini").string());
    EXPECT_EQ(settings.Get<std::string>("start_scene_asset"), ""); EXPECT_EQ(settings.Get<std::string>("start_scene_subasset"), "");
    EXPECT_TRUE(std::filesystem::exists(project.install / "Data/User/Assets/Main.scene"));
}
TEST(ECSPlayer, StagedValidationRejectsStartupIdentityThatNamesAPrefab)
{
    Project project; project.AddDependency(); project.Cook(); project.Stage();
    Project::Write(project.install / "Data/User/Game.ini", "start_scene_asset=" + project.prefabId.ToString() + "\nstart_scene_subasset=ecs-prefab:main\n");
    std::string error; EXPECT_FALSE(Build::Detail::VerifyStagedContent(project.context, project.report, project.packages, &error));
    EXPECT_NE(error.find("ECS start-scene identity"), std::string::npos);
}
TEST(ECSPlayer, DevelopmentManifestRecognizesNativeECSTypesAndConfigurationRejectsWrongKind)
{
    Project project; project.Cook(); auto database = Startup::LoadRuntimeAssetDatabaseForProjectSettings(project.root / "Game.ini");
    ASSERT_TRUE(database); EXPECT_NE(database->Resolve({project.sceneId, "ecs-scene:main"}), nullptr);
    EXPECT_NE(database->Resolve({project.prefabId, "ecs-prefab:main"}), nullptr);
    Scripting::RuntimePlaySession session;
    auto snapshot = std::make_shared<const Engine::Assets::RuntimeAssetDatabase>(std::move(*database));
    EXPECT_THROW(Startup::ConfigureECSStartScene(session, snapshot, project.root, {project.prefabId, "ecs-prefab:main"}), std::invalid_argument);
    EXPECT_THROW(Startup::ConfigureECSStartScene(session, {}, project.root, {project.sceneId, {}}), std::invalid_argument);
    EXPECT_EQ(session.GetECS(), nullptr); EXPECT_FALSE(session.IsActive());
}
TEST(ECSPlayer, RejectsStaleAndDifferentTargetCookedStartScenes)
{
    Project project; project.Cook(); CA::ArtifactDatabase database; ASSERT_TRUE(database.Load(project.databasePath));
    database.MarkStatus(project.sceneId, CA::ArtifactRecordStatus::Stale); ASSERT_TRUE(database.Save(project.databasePath));
    EXPECT_FALSE(project.Collect()); project.report = {};
    database.MarkStatus(project.sceneId, CA::ArtifactRecordStatus::UpToDate); ASSERT_TRUE(database.Save(project.databasePath));
    project.context.profile.target.artifactPlatform = "linux"; EXPECT_FALSE(project.Collect());
}
}
