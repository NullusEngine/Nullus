#include "JobTestSupport.h"
#include <Engine/ECS/ECSAssets.h>
#include <Engine/ECS/RuntimeWorld.h>
#include <Engine/ECS/TransformHierarchy.h>
#include <Engine/Assets/RuntimeManifestSerialization.h>
#include <Cook/CookTypes.h>
#include <Build/BuildDependencyCollector.h>
#include <Build/BuildReport.h>
#include <Core/Assets/AssetMeta.h>
#include <Core/Assets/ContentSource.h>
#include <Core/Assets/NativeArtifactContainer.h>
#include <Core/Assets/NpakArchive.h>
#include <gtest/gtest.h>
#include <array>
#include <algorithm>
#include <fstream>
#include <set>
#if defined(NLS_ECS_ASSET_EDITOR_TESTS)
#include <Assets/AssetDatabaseFacade.h>
#include <Assets/ImportProgressTracker.h>
#endif

namespace
{
using namespace NLS;
using namespace NLS::ECS;
using namespace NLS::Engine::ECSIntegration;
namespace CA = NLS::Core::Assets;
namespace EA = NLS::Engine::Assets;
struct ResourceRef { std::string guid, version; };
void Register(World& world)
{
    RegisterTransformSystem(world);
    world.RegisterComponent<ResourceRef>("asset-test.ResourceRef");
    RegisterComponentCodec<ResourceRef>(world.GetRegistry(),
        [](const ResourceRef& value, const EntityWriteContext& context)
        { return ComponentJson{{"guid", context.Resource(value.guid, value.version)}, {"version", value.version}}; },
        [](ResourceRef& value, const ComponentJson& json, const EntityReadContext& context)
        { value = {context.Resource(json.at("guid").get<std::string>()), json.at("version").get<std::string>()}; });
}
struct Fixture
{
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::path("tmp") / ("ecs-assets-" + Guid::New().ToString()));
    std::filesystem::path assets = root / "Assets";
    std::filesystem::path databasePath = root / "Library" / "ArtifactDB";
    NLS::Cook::CookOptions options;
    Fixture()
    {
        std::filesystem::create_directories(assets);
        options.projectRoot = root; options.assetsRoot = assets; options.artifactDatabasePath = databasePath;
        options.target.artifactPlatform = "win64";
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    static void Write(const std::filesystem::path& path, std::string_view text)
    { std::ofstream file(path, std::ios::binary); file.write(text.data(), static_cast<std::streamsize>(text.size())); }
    CA::AssetId Meta(const std::filesystem::path& path)
    {
        auto meta = CA::AssetMeta::CreateForAsset(path);
        if (!meta.Save(CA::GetAssetMetaPath(path))) throw std::runtime_error("Test meta write failed");
        return meta.id;
    }
    WorldDocument Document(DocumentKind kind = DocumentKind::Scene, ResourceRef resource = {})
    {
        World world; Register(world); const auto parent = world.CreateEntity(), child = world.CreateEntity();
        AddTransform(world, parent, {{7, 0, 0}}); AddTransform(world, child, {{3, 0, 0}}, parent);
        if (!resource.guid.empty()) world.AddComponent<ResourceRef>(parent, resource);
        const std::array records{PersistentEntity{parent, "parent", "author-parent"}, PersistentEntity{child, "child", "author-child"}};
        return WorldArchive::Capture(world, records, kind);
    }
    std::pair<std::filesystem::path, CA::AssetId> Scene(std::string name = "World", ResourceRef resource = {})
    {
        auto path = assets / (name + ".ecsscene"); WorldArchive::WriteFile(path, Document(DocumentKind::Scene, std::move(resource)));
        return {path, Meta(path)};
    }
    std::pair<std::filesystem::path, CA::AssetId> Prefab(std::string name = "Prop", ResourceRef resource = {})
    {
        auto path = assets / (name + ".ecsprefab"); WorldArchive::WriteFile(path, Document(DocumentKind::Prefab, std::move(resource)));
        return {path, Meta(path)};
    }
    std::pair<std::filesystem::path, CA::AssetId> Texture()
    {
        auto path = assets / "Pixel.tga";
        const std::array<uint8_t, 22> tga{0,0,2,0,0,0,0,0,0,0,0,0,1,0,1,0,32,8,0,0,255,255};
        { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(tga.data()), tga.size()); }
        return {path, Meta(path)};
    }
    CA::ArtifactDatabase Database() const
    { CA::ArtifactDatabase database; if (!database.Load(databasePath)) throw std::runtime_error(database.GetLastError()); return database; }
    NLS::Cook::CookSummary Cook(const std::vector<std::filesystem::path>& paths) const { return NLS::Cook::AssetCooker::CookAssets(options, paths); }
    CA::ArtifactManifest Manifest(CA::AssetId id) const { return Database().BuildManifestForSource(id, options.target.artifactPlatform).value(); }
    Build::BuildProfile Profile(const std::filesystem::path& kept)
    {
        const auto path = assets / "Empty.scene";
        const ComponentJson scene{{"format", "Nullus.ObjectGraph.Scene"}, {"documentId", Guid::New().ToString()},
            {"root", Guid::New().ToString()}, {"objects", ComponentJson::array()}};
        Write(path, scene.dump()); Meta(path);
        Build::BuildProfile profile; profile.id = "ecs-test"; profile.startScene = "Empty.scene";
        profile.target.artifactPlatform = "win64"; profile.keepAssetPaths.push_back(kept.filename().generic_string());
        return profile;
    }
};
TEST(ECSAssets, NativeTypesPreserveLegacyTypesAndRoundTripMetadata)
{
    EXPECT_EQ(CA::InferAssetType("WORLD.ECSSCENE"), CA::AssetType::ECSScene);
    EXPECT_EQ(CA::InferAssetType("Prop.ecsprefab"), CA::AssetType::ECSPrefab);
    EXPECT_EQ(CA::AssetTypeFromString(CA::ToString(CA::AssetType::ECSScene)), CA::AssetType::ECSScene);
    EXPECT_EQ(CA::AssetTypeFromString(CA::ToString(CA::AssetType::ECSPrefab)), CA::AssetType::ECSPrefab);
    EXPECT_EQ(CA::InferImporterId(CA::AssetType::ECSScene), "ecs-scene");
    EXPECT_EQ(CA::InferAssetType("Prop.prefab"), CA::AssetType::Prefab);
    for (const auto type : {CA::ArtifactType::ECSScene, CA::ArtifactType::ECSPrefab})
    {
        CA::NativeArtifactMetadata metadata; metadata.artifactType = type; metadata.schemaVersion = 1;
        metadata.schemaName = type == CA::ArtifactType::ECSScene ? "ecs-scene" : "ecs-prefab";
        metadata.sourceAssetId = CA::AssetId::New(); metadata.subAssetKey = metadata.schemaName + ":main";
        metadata.importerId = metadata.schemaName; metadata.importerVersion = 1; metadata.targetPlatform = "win64";
        auto bytes = CA::WriteNativeArtifactContainer(metadata, std::vector<uint8_t>{1,2,3});
        const auto decoded = CA::ReadNativeArtifactContainer(bytes, type, 1);
        ASSERT_TRUE(decoded); EXPECT_EQ(decoded->metadata.artifactType, type);
    }
}
TEST(ECSAssets, ColdCookPublishesRealTextureAndECSArtifactsWithNativeDependencies)
{
    Fixture fixture; const auto [texturePath, textureId] = fixture.Texture();
    const auto [scenePath, sceneId] = fixture.Scene("World", {textureId.ToString(), {}});
    const auto result = fixture.Cook({scenePath, texturePath});
    ASSERT_TRUE(result.Succeeded()) << result.FormatSummary(); EXPECT_EQ(result.cooked, 2u);
    auto database = fixture.Database(); auto manifest = fixture.Manifest(sceneId);
    ASSERT_TRUE(manifest.FindPrimaryArtifact()); EXPECT_EQ(manifest.FindPrimaryArtifact()->artifactType, CA::ArtifactType::ECSScene);
    EXPECT_TRUE(std::any_of(manifest.dependencies.begin(), manifest.dependencies.end(), [&](const auto& dependency)
    { return dependency.kind == CA::AssetDependencyKind::ImportedArtifact && dependency.value == textureId.ToString() && !dependency.hashOrVersion.empty(); }));
    EXPECT_TRUE(AreECSAssetResourcesCurrent(manifest, database, "win64", fixture.root));
    auto loader = ECSAssetLoader::FromArtifactDatabase(database, fixture.root, "win64");
    const auto document = loader.Read({sceneId, "ecs-scene:main"}); EXPECT_EQ(document.entities.size(), 2u);
    EXPECT_TRUE(loader.ResolveResource(document.resources.at(0)));
}
TEST(ECSAssets, UnchangedCookSkipsAndResourceChangesInvalidateOnlyTheECSConsumer)
{
    Fixture fixture; const auto [texturePath, textureId] = fixture.Texture();
    const auto [scenePath, sceneId] = fixture.Scene("World", {textureId.ToString(), {}});
    ASSERT_TRUE(fixture.Cook({scenePath, texturePath}).Succeeded()); const auto old = fixture.Manifest(sceneId).FindPrimaryArtifact()->contentHash;
    const auto unchanged = fixture.Cook({scenePath, texturePath}); ASSERT_TRUE(unchanged.Succeeded()); EXPECT_EQ(unchanged.skipped, 2u);
    { std::fstream file(texturePath, std::ios::binary | std::ios::in | std::ios::out); file.seekp(18); const char green = 100; file.write(&green, 1); }
    const auto changed = fixture.Cook({scenePath, texturePath}); ASSERT_TRUE(changed.Succeeded()) << changed.FormatSummary();
    EXPECT_EQ(changed.cooked, 2u); EXPECT_NE(fixture.Manifest(sceneId).FindPrimaryArtifact()->contentHash, old);
}
TEST(ECSAssets, PinnedResourceVersionRejectsChangesAndRetainsLastPublishedArtifact)
{
    Fixture fixture; const auto [texturePath, textureId] = fixture.Texture(); ASSERT_TRUE(fixture.Cook({texturePath}).Succeeded());
    const auto version = fixture.Manifest(textureId).FindPrimaryArtifact()->contentHash;
    const auto [scenePath, sceneId] = fixture.Scene("World", {textureId.ToString(), version}); ASSERT_TRUE(fixture.Cook({scenePath}).Succeeded());
    const auto previous = fixture.Manifest(sceneId).FindPrimaryArtifact()->contentHash;
    { std::fstream file(texturePath, std::ios::binary | std::ios::in | std::ios::out); file.seekp(18); const char green = 100; file.write(&green, 1); }
    ASSERT_TRUE(fixture.Cook({texturePath}).Succeeded()); EXPECT_FALSE(fixture.Cook({scenePath}).Succeeded());
    EXPECT_EQ(fixture.Manifest(sceneId).FindPrimaryArtifact()->contentHash, previous);
}
TEST(ECSAssets, MissingStaleWrongTargetAndInvalidResourcesFailBeforePublishing)
{
    Fixture fixture; const auto [path, id] = fixture.Scene("Missing", {Guid::New().ToString(), {}});
    EXPECT_FALSE(fixture.Cook({path}).Succeeded());
    CA::ArtifactDatabase empty;
    EXPECT_THROW(CompileECSAsset(WorldArchive::Encode(fixture.Document(DocumentKind::Scene, {"not-a-guid", {}})),
        DocumentKind::Scene, empty, "win64", fixture.root), std::invalid_argument);
    const auto [texturePath, textureId] = fixture.Texture(); ASSERT_TRUE(fixture.Cook({texturePath}).Succeeded());
    auto database = fixture.Database(); auto document = fixture.Document(DocumentKind::Scene, {textureId.ToString(), {}});
    EXPECT_THROW(CompileECSAsset(WorldArchive::Encode(document), DocumentKind::Scene, database, "other-target", fixture.root), std::invalid_argument);
    database.MarkStatus(textureId, CA::ArtifactRecordStatus::Stale);
    EXPECT_THROW(CompileECSAsset(WorldArchive::Encode(document), DocumentKind::Scene, database, "win64", fixture.root), std::invalid_argument);
}
TEST(ECSAssets, ReverseOrderedECSInputsAreCookedByDependencyAndCyclesAreDiagnosed)
{
    Fixture fixture; const auto [prefabPath, prefabId] = fixture.Prefab();
    const auto [scenePath, sceneId] = fixture.Scene("World", {prefabId.ToString(), {}});
    const auto result = fixture.Cook({scenePath, prefabPath}); ASSERT_TRUE(result.Succeeded()) << result.FormatSummary(); EXPECT_EQ(result.cooked, 2u);
    auto cycle = fixture.Document(DocumentKind::Prefab, {sceneId.ToString(), {}}); WorldArchive::WriteFile(prefabPath, cycle);
    const auto previous = fixture.Manifest(sceneId).FindPrimaryArtifact()->contentHash;
    const auto rejected = fixture.Cook({scenePath, prefabPath}); EXPECT_FALSE(rejected.Succeeded()); EXPECT_EQ(rejected.cooked, 0u);
    EXPECT_EQ(fixture.Manifest(sceneId).FindPrimaryArtifact()->contentHash, previous);
}
TEST(ECSAssets, WrongDocumentKindAndCorruptSourceKeepThePreviousArtifact)
{
    Fixture fixture; const auto [path, id] = fixture.Scene(); ASSERT_TRUE(fixture.Cook({path}).Succeeded());
    const auto previous = fixture.Manifest(id).FindPrimaryArtifact()->contentHash;
    WorldArchive::WriteFile(path, fixture.Document(DocumentKind::Prefab)); EXPECT_FALSE(fixture.Cook({path}).Succeeded());
    EXPECT_EQ(fixture.Manifest(id).FindPrimaryArtifact()->contentHash, previous);
    Fixture::Write(path, "{invalid"); EXPECT_FALSE(fixture.Cook({path}).Succeeded());
    EXPECT_EQ(fixture.Manifest(id).FindPrimaryArtifact()->contentHash, previous);
}
TEST(ECSAssets, RuntimeAssetPrefabInstancesRemapReferencesAndSupportOverrides)
{
    Fixture fixture; const auto [path, id] = fixture.Prefab(); ASSERT_TRUE(fixture.Cook({path}).Succeeded());
    auto loader = ECSAssetLoader::FromArtifactDatabase(fixture.Database(), fixture.root, "win64");
    RuntimeWorld runtime; Register(runtime.GetWorld()); runtime.ConfigureAssets(std::make_shared<ECSAssetLoader>(loader));
    const auto first = runtime.InstantiatePrefabAsset({id, {}});
    PrefabOverride change; change.entityId = "parent"; change.type = runtime.GetWorld().GetComponentType<LocalTransform>().id;
    change.field = "/position/0"; change.value = 20;
    const auto second = runtime.InstantiatePrefabAsset({id, {}}, std::span<const PrefabOverride>(&change, 1));
    EXPECT_NE(first.Find("parent"), second.Find("parent"));
    EXPECT_EQ(runtime.GetWorld().ReadComponent<Parent>(first.Find("child")).entity, first.Find("parent"));
    EXPECT_EQ(runtime.GetWorld().ReadComponent<Parent>(second.Find("child")).entity, second.Find("parent"));
    runtime.RunPhase(SystemPhase::LateUpdate, {});
    EXPECT_FLOAT_EQ(runtime.GetWorld().ReadComponent<WorldTransform>(first.Find("child")).matrix[3], 10);
    EXPECT_FLOAT_EQ(runtime.GetWorld().ReadComponent<WorldTransform>(second.Find("child")).matrix[3], 23);
}
TEST(ECSAssets, RealPackageLoadsThroughRuntimeWorldAfterAuthoringAndArtifactFoldersAreRemoved)
{
    ECSTest::JobSystemScope jobs; Fixture fixture;
    const auto [texturePath, textureId] = fixture.Texture(); const auto [scenePath, sceneId] = fixture.Scene("World", {textureId.ToString(), {}});
    ASSERT_TRUE(fixture.Cook({scenePath, texturePath}).Succeeded());
    const auto profile = fixture.Profile(scenePath); Build::DependencyCollectionResult collected; Build::BuildReport report;
    ASSERT_TRUE(Build::BuildDependencyCollector::Collect(fixture.root, fixture.assets, profile, fixture.databasePath, collected, report));
    ASSERT_EQ(collected.manifest.entries.size(), 2u); EXPECT_EQ(collected.artifacts.size(), 2u);
    auto decodedManifest = EA::DeserializeRuntimeAssetManifest(EA::SerializeRuntimeAssetManifest(collected.manifest));
    ASSERT_TRUE(decodedManifest); ASSERT_EQ(decodedManifest->entries.size(), 2u);
    const auto packagePath = fixture.root / "base.npak"; CA::NpakWriter writer;
    for (auto& entry : decodedManifest->entries)
    {
        const auto host = Build::BuildDependencyCollector::ResolveArtifactHostPath(fixture.root, entry.artifactPath);
        auto portable = CA::TryMakePortableContentArtifactPath(entry.artifactPath);
        if (portable.starts_with("Library/")) portable.erase(0, 8);
        ASSERT_TRUE(writer.AddFileFromDisk(portable, host)); entry.artifactPath = portable;
    }
    std::string error; ASSERT_TRUE(writer.Write(packagePath, {}, &error)) << error;
    auto opened = CA::NpakArchive::Open(packagePath, &error); ASSERT_TRUE(opened) << error;
    auto package = std::make_shared<CA::NpakArchive>(std::move(*opened));
    auto source = std::make_shared<CA::PackageContentSource>(package); const auto install = fixture.root / "Install";
    CA::ContentSourceTable::Get().Mount(install.generic_string(), source);
    struct Unmount { std::filesystem::path root; const CA::IContentSource* source; ~Unmount() { CA::ContentSourceTable::Get().Unmount(root, source); } } unmount{install, source.get()};
    std::filesystem::remove_all(fixture.root / "Library"); std::filesystem::remove_all(fixture.assets);
    RuntimeWorld runtime; Register(runtime.GetWorld()); runtime.ConfigureAssets(std::make_shared<EA::RuntimeAssetDatabase>(std::move(*decodedManifest)), install);
    const auto pending = runtime.RequestSceneAsset(1, {sceneId, "ecs-scene:main"});
    const auto loaded = runtime.Streaming().Wait(pending); ASSERT_EQ(loaded.status, SceneLoadStatus::Loaded) << loaded.error;
    EXPECT_TRUE(loaded.parsedOnBackgroundWorker); EXPECT_EQ(runtime.GetWorld().GetEntityCount(), 2u);
    const auto parent = runtime.Streaming().Find(1, "parent"), child = runtime.Streaming().Find(1, "child");
    EXPECT_EQ(runtime.GetWorld().ReadComponent<ResourceRef>(parent).guid, textureId.ToString());
    runtime.RunPhase(SystemPhase::LateUpdate, {}); EXPECT_FLOAT_EQ(runtime.GetWorld().ReadComponent<WorldTransform>(child).matrix[3], 10);
    EXPECT_EQ(runtime.Streaming().Unload(1), 2u); EXPECT_FALSE(runtime.GetWorld().IsAlive(child));
}
TEST(ECSAssets, NativeLoaderRejectsCorruptBytesAndOversizedPayload)
{
    Fixture fixture; const auto [path, id] = fixture.Scene(); ASSERT_TRUE(fixture.Cook({path}).Succeeded());
    auto loader = ECSAssetLoader::FromArtifactDatabase(fixture.Database(), fixture.root, "win64");
    ArchiveLimits limits; limits.maxBytes = 16; EXPECT_THROW(loader.Read({id, {}}, limits), std::length_error);
    const auto artifact = fixture.Manifest(id).FindPrimaryArtifact()->artifactPath;
    const auto host = Build::BuildDependencyCollector::ResolveArtifactHostPath(fixture.root, artifact);
    { std::fstream file(host, std::ios::binary | std::ios::in | std::ios::out); file.seekp(-1, std::ios::end); const char invalid = '!'; file.write(&invalid, 1); }
    EXPECT_THROW(loader.Read({id, {}}), std::invalid_argument);
}
TEST(ECSAssets, RuntimeResourceResolverRejectsMissingVersionsAndMissingPayloads)
{
    Fixture fixture; const auto [path, id] = fixture.Prefab(); ASSERT_TRUE(fixture.Cook({path}).Succeeded());
    auto loader = ECSAssetLoader::FromArtifactDatabase(fixture.Database(), fixture.root, "win64");
    const auto dependency = loader.DescribeResource({id, {}}); EXPECT_TRUE(loader.ResolveResource(dependency));
    EXPECT_FALSE(loader.ResolveResource({dependency.guid, "different-version"})); EXPECT_FALSE(loader.ResolveResource({"invalid", {}}));
    std::filesystem::remove(Build::BuildDependencyCollector::ResolveArtifactHostPath(fixture.root, fixture.Manifest(id).FindPrimaryArtifact()->artifactPath));
    EXPECT_FALSE(loader.ResolveResource(dependency));
}
TEST(ECSAssets, NativeLoaderRejectsWrongIdentityAndFutureContainerSchema)
{
    Fixture fixture; const auto [path, id] = fixture.Scene(); ASSERT_TRUE(fixture.Cook({path}).Succeeded());
    const auto artifact = *fixture.Manifest(id).FindPrimaryArtifact();
    EA::RuntimeAssetManifest manifest; manifest.targetPlatform = "win64";
    const auto wrongId = CA::AssetId::New();
    manifest.entries.push_back({wrongId, artifact.subAssetKey, artifact.artifactType, artifact.loaderId, artifact.artifactPath, artifact.contentHash, {}});
    ECSAssetLoader wrongIdentity(std::make_shared<EA::RuntimeAssetDatabase>(manifest), fixture.root);
    EXPECT_THROW(wrongIdentity.Read({wrongId, {}}), std::invalid_argument);
    manifest.entries[0].assetId = id; manifest.targetPlatform = "other-target";
    ECSAssetLoader wrongTarget(std::make_shared<EA::RuntimeAssetDatabase>(manifest), fixture.root);
    EXPECT_THROW(wrongTarget.Read({id, {}}), std::invalid_argument);
    CA::NativeArtifactMetadata metadata; metadata.artifactType = CA::ArtifactType::ECSScene; metadata.schemaName = "ecs-scene";
    metadata.schemaVersion = 2; metadata.sourceAssetId = id; metadata.subAssetKey = "ecs-scene:main"; metadata.targetPlatform = "win64";
    metadata.importerId = "ecs-scene"; metadata.importerVersion = 1;
    const auto text = WorldArchive::Encode(fixture.Document());
    const auto bytes = CA::WriteNativeArtifactContainer(metadata, std::vector<uint8_t>(text.begin(), text.end()));
    const auto name = CA::BuildArtifactStorageFileName(bytes.data(), bytes.size());
    const auto relative = std::filesystem::path("Library/Artifacts") / CA::BuildArtifactStorageRelativePath(name);
    const auto host = fixture.root / relative; std::filesystem::create_directories(host.parent_path());
    Fixture::Write(host, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    manifest.targetPlatform = "win64"; manifest.entries[0].artifactPath = relative.generic_string(); manifest.entries[0].contentHash = "sha256:" + name;
    ECSAssetLoader future(std::make_shared<EA::RuntimeAssetDatabase>(std::move(manifest)), fixture.root);
    EXPECT_THROW(future.Read({id, {}}), std::invalid_argument);
}
TEST(ECSAssets, RuntimeLoaderUsesExistingArtifactAuthorization)
{
    Fixture fixture; const auto [path, id] = fixture.Scene(); ASSERT_TRUE(fixture.Cook({path}).Succeeded());
    auto loader = ECSAssetLoader::FromArtifactDatabase(fixture.Database(), fixture.root, "win64");
    CA::ClearRuntimeArtifactAuthorization(); CA::SetRuntimeArtifactAuthorizationEnabled(true);
    struct Reset { ~Reset() { CA::SetRuntimeArtifactAuthorizationEnabled(false); CA::ClearRuntimeArtifactAuthorization(); } } reset;
    EXPECT_THROW(loader.Read({id, {}}), std::invalid_argument);
    CA::RegisterRuntimeAuthorizedArtifactPath(fixture.Manifest(id).FindPrimaryArtifact()->artifactPath);
    EXPECT_EQ(loader.Read({id, {}}).entities.size(), 2u);
}
#if defined(NLS_ECS_ASSET_EDITOR_TESTS)
std::string EditorDiagnostics(const Editor::Assets::AssetDatabaseFacade& database)
{
    std::string result;
    for (const auto& diagnostic : database.GetDiagnostics()) result += diagnostic.code + ": " + diagnostic.message + "\n";
    return result;
}
TEST(ECSAssetEditor, NativeImportPersistsBothECSTypesAndReloadsTheirManifests)
{
    Fixture fixture; const auto [scenePath, sceneId] = fixture.Scene(); const auto [prefabPath, prefabId] = fixture.Prefab();
    {
        Editor::Assets::AssetDatabaseFacade database(std::vector<std::filesystem::path>{fixture.root});
        ASSERT_TRUE(database.Refresh()); ASSERT_TRUE(database.ImportAsset("Assets/World.ecsscene")) << EditorDiagnostics(database);
        ASSERT_TRUE(database.ImportAsset("Assets/Prop.ecsprefab")) << EditorDiagnostics(database);
    }
    auto database = fixture.Database(); auto scene = database.BuildManifestForSource(sceneId, "editor"), prefab = database.BuildManifestForSource(prefabId, "editor");
    ASSERT_TRUE(scene); ASSERT_TRUE(prefab); EXPECT_EQ(scene->FindPrimaryArtifact()->artifactType, CA::ArtifactType::ECSScene);
    EXPECT_EQ(prefab->FindPrimaryArtifact()->artifactType, CA::ArtifactType::ECSPrefab);
    auto loader = ECSAssetLoader::FromArtifactDatabase(database, fixture.root, "editor"); EXPECT_EQ(loader.Read({sceneId, {}}).entities.size(), 2u);
    Editor::Assets::AssetDatabaseFacade reloaded(std::vector<std::filesystem::path>{fixture.root}); ASSERT_TRUE(reloaded.Refresh());
    EXPECT_FALSE(reloaded.ResolveArtifactPathAtPath("Assets/World.ecsscene", "ecs-scene:main").empty());
}
TEST(ECSAssetEditor, FailedReimportKeepsTheLastGoodArtifact)
{
    Fixture fixture; const auto [path, id] = fixture.Scene(); Editor::Assets::AssetDatabaseFacade database(std::vector<std::filesystem::path>{fixture.root});
    ASSERT_TRUE(database.Refresh()); ASSERT_TRUE(database.ImportAsset("Assets/World.ecsscene")) << EditorDiagnostics(database);
    const auto previous = fixture.Database().BuildManifestForSource(id, "editor")->FindPrimaryArtifact()->contentHash;
    WorldArchive::WriteFile(path, fixture.Document(DocumentKind::Prefab)); EXPECT_FALSE(database.ReimportAsset("Assets/World.ecsscene"));
    EXPECT_EQ(fixture.Database().BuildManifestForSource(id, "editor")->FindPrimaryArtifact()->contentHash, previous);
}
TEST(ECSAssetEditor, DependentImportsSeeUnflushedArtifactDatabaseUpdates)
{
    Fixture fixture; const auto [prefabPath, prefabId] = fixture.Prefab(); const auto [scenePath, sceneId] = fixture.Scene("World", {prefabId.ToString(), {}});
    Editor::Assets::AssetDatabaseFacade database(std::vector<std::filesystem::path>{fixture.root}); ASSERT_TRUE(database.Refresh());
    Editor::Assets::ImportProgressTracker progress;
    database.BeginArtifactDatabaseFlushBatch();
    const auto prefabImported = database.ImportAssetFromCurrentDatabase("Assets/Prop.ecsprefab", progress, 2);
    const auto prefabDiagnostics = EditorDiagnostics(database);
    CA::ArtifactDatabase disk;
    const bool prefabPersistedEarly = disk.Load(fixture.databasePath) && disk.BuildManifestForSource(prefabId, "editor").has_value();
    const auto sceneImported = database.ImportAssetFromCurrentDatabase("Assets/World.ecsscene", progress, 2);
    const auto sceneDiagnostics = EditorDiagnostics(database);
    const auto flushed = database.EndArtifactDatabaseFlushBatch();
    ASSERT_TRUE(prefabImported) << prefabDiagnostics; ASSERT_TRUE(sceneImported) << sceneDiagnostics; ASSERT_TRUE(flushed);
    EXPECT_FALSE(prefabPersistedEarly);
    EXPECT_TRUE(fixture.Database().BuildManifestForSource(sceneId, "editor"));
}
#endif
}
