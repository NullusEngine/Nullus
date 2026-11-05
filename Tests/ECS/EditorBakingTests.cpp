#include <ECS/SceneBakingService.h>
#include <ECS/BakingAssetImport.h>
#include <Panels/HierarchyUndo.h>
#include <Undo/ObjectUndoTarget.h>
#include <Assets/AssetDatabaseFacade.h>
#include <Engine/ECS/ECSAssets.h>
#include <Engine/ECS/TransformHierarchy.h>
#include <Engine/GameObject.h>
#include <Engine/Components/TransformComponent.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/SceneSystem/SceneManager.h>
#include <Engine/Serialize/ObjectGraphSerializer.h>
#include <Core/Assets/AssetMeta.h>
#include <gtest/gtest.h>

namespace
{
using namespace NLS;
using namespace NLS::ECS;
using namespace NLS::Engine::ECSIntegration;
using NLS::Editor::ECSAuthoring::SceneBakingService;
namespace Undo = NLS::Editor::Undo;
namespace Hierarchy = NLS::Editor::Panels::HierarchyUndo;
using Scene = NLS::Engine::SceneSystem::Scene;
using SceneClock = SceneBakingService::Clock;
using namespace std::chrono_literals;
struct BakedName { std::string value; };
struct BakedResource { std::string guid; };
struct ExportFixture
{
    std::filesystem::path parent = std::filesystem::weakly_canonical(std::filesystem::absolute("tmp"));
    std::filesystem::path root = parent / ("ecs-editor-bake-" + Guid::New().ToString());
    ExportFixture() { std::filesystem::create_directories(root / "Assets"); }
    ~ExportFixture()
    {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(root, error);
        if (!error && resolved.parent_path() == parent &&
            resolved.filename().string().starts_with("ecs-editor-bake-"))
            std::filesystem::remove_all(resolved, error);
    }
};
Undo::UndoScopeId Scope()
{
    return {Undo::UndoScopeKind::Scene, "ecs-editor-baking-" + Guid::New().ToString(), 1};
}
void RegisterNames(SceneBakingService& service)
{
    auto& world = service.GetPreview().GetWorld();
    world.RegisterComponent<BakedName>("ecs-editor-test.Name");
    RegisterComponentCodec<BakedName>(world.GetRegistry(),
        [](const BakedName& name, const auto&) { return ComponentJson{{"value", name.value}}; },
        [](BakedName& name, const ComponentJson& json, const auto&) { name.value = json.at("value").get<std::string>(); });
    service.GetPipeline().RegisterBaker({"ecs-editor-test.Name", 1,
        [](const Engine::GameObject& object, const auto&) -> std::optional<ComponentJson>
        { return ComponentJson{{"value", object.GetName()}}; },
        [](const ComponentJson& input, BakingOutput& output)
        {
            if (input.at("value") == "invalid") throw std::invalid_argument("Test authoring name is invalid");
            output.AddComponent<BakedName>(output.Primary(), input);
        }});
}
float X(SceneBakingService& service, const Engine::Serialize::ObjectId& id)
{
    auto& preview = service.GetPreview();
    return preview.GetWorld().ReadComponent<WorldTransform>(preview.FindAuthoring(id)).matrix[3];
}

TEST(ECSEditorBaking, ManualRequestsCoalesceAndUnchangedArtifactsRetainPreviewHandles)
{
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    auto& object = scene.CreateGameObject("object");
    const auto now = SceneClock::now();
    EXPECT_FALSE(service.Tick(&scene, true, now));
    EXPECT_EQ(service.GetBakeCount(), 0u);
    service.RequestBake(BakingMode::Incremental); service.RequestBake(BakingMode::Full);
    ASSERT_TRUE(service.Tick(&scene, true, now));
    ASSERT_TRUE(service.GetLastResult().succeeded);
    const auto id = CaptureSceneBakingSources(scene).at(0).id;
    const auto entity = service.GetPreview().FindAuthoring(id);
    service.RequestBake(BakingMode::Incremental);
    ASSERT_TRUE(service.Tick(&scene, true, now));
    EXPECT_EQ(service.GetLastResult().statistics.rebuilt, 0u);
    EXPECT_EQ(service.GetPreview().FindAuthoring(id), entity);
    EXPECT_FALSE(service.Tick(&scene, true, now + 1s));
    EXPECT_EQ(service.GetBakeCount(), 2u);
    static_cast<void>(object);
}

TEST(ECSEditorBaking, BoundedLivePollingDetectsDirectAuthoringChangesWithoutUndoEvents)
{
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    auto& object = scene.CreateGameObject("object");
    const auto now = SceneClock::now(); service.SetLive(true);
    ASSERT_TRUE(service.Tick(&scene, true, now));
    const auto id = CaptureSceneBakingSources(scene).at(0).id;
    object.GetTransform()->SetLocalPosition({9, 0, 0});
    EXPECT_FALSE(service.Tick(&scene, true, now + 100ms));
    EXPECT_FLOAT_EQ(X(service, id), 0);
    ASSERT_TRUE(service.Tick(&scene, true, now + 250ms));
    EXPECT_FLOAT_EQ(X(service, id), 9);
    service.SetLive(false);
    object.GetTransform()->SetLocalPosition({12, 0, 0});
    EXPECT_FALSE(service.Tick(&scene, true, now + 1s));
    EXPECT_FLOAT_EQ(X(service, id), 9);
}

TEST(ECSEditorBaking, RealRenameUndoRedoInvalidatesOnlyTheAffectedBaker)
{
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    auto& object = scene.CreateGameObject("before"); RegisterNames(service);
    const auto now = SceneClock::now(); service.SetLive(true);
    ASSERT_TRUE(service.Tick(&scene, true, now));
    const auto id = CaptureSceneBakingSources(scene).at(0).id;
    const auto read = [&] { auto& preview = service.GetPreview();
        return preview.GetWorld().ReadComponent<BakedName>(preview.FindAuthoring(id)).value; };
    ASSERT_TRUE(Hierarchy::RenameGameObject(undo, object, "after", Scope()).Succeeded());
    ASSERT_TRUE(service.Tick(&scene, true, now));
    EXPECT_EQ(read(), "after"); EXPECT_EQ(service.GetLastResult().statistics.rebuilt, 1u);
    EXPECT_EQ(service.GetLastResult().statistics.reused, 1u);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); ASSERT_TRUE(service.Tick(&scene, true, now));
    EXPECT_EQ(read(), "before");
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); ASSERT_TRUE(service.Tick(&scene, true, now));
    EXPECT_EQ(read(), "after");
}

TEST(ECSEditorBaking, ReparentUndoRedoUpdatesNativeHierarchyWithoutChangingAuthoringIds)
{
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    auto& first = scene.CreateGameObject("first"); auto& second = scene.CreateGameObject("second");
    auto& child = scene.CreateGameObject("child"); child.SetParent(first);
    first.GetTransform()->SetLocalPosition({4, 0, 0}); second.GetTransform()->SetLocalPosition({10, 0, 0});
    child.GetTransform()->SetLocalPosition({2, 0, 0});
    const auto id = CaptureSceneBakingSources(scene).at(2).id;
    const auto now = SceneClock::now(); service.SetLive(true); ASSERT_TRUE(service.Tick(&scene, true, now));
    EXPECT_FLOAT_EQ(X(service, id), 6);
    ASSERT_TRUE(Hierarchy::ReparentGameObject(undo, child, &second, Scope()).Succeeded());
    ASSERT_TRUE(service.Tick(&scene, true, now)); EXPECT_FLOAT_EQ(X(service, id), 12);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); ASSERT_TRUE(service.Tick(&scene, true, now)); EXPECT_FLOAT_EQ(X(service, id), 6);
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); ASSERT_TRUE(service.Tick(&scene, true, now)); EXPECT_FLOAT_EQ(X(service, id), 12);
    EXPECT_EQ(CaptureSceneBakingSources(scene).at(2).id, id);
}

TEST(ECSEditorBaking, DeleteUndoRedoRestoresTheSamePersistentIdsAndPreviewMapping)
{
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    auto& root = scene.CreateGameObject("root"); auto& child = scene.CreateGameObject("child"); child.SetParent(root);
    const auto sources = CaptureSceneBakingSources(scene);
    const auto now = SceneClock::now(); service.SetLive(true); ASSERT_TRUE(service.Tick(&scene, true, now));
    ASSERT_TRUE(Hierarchy::DeleteGameObject(undo, root, Scope()).Succeeded());
    ASSERT_TRUE(service.Tick(&scene, true, now)); EXPECT_EQ(service.GetPreview().GetWorld().GetEntityCount(), 0u);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    ASSERT_TRUE(service.Tick(&scene, true, now)); ASSERT_TRUE(service.GetLastResult().succeeded);
    EXPECT_EQ(service.GetPreview().GetWorld().GetEntityCount(), 2u);
    for (const auto& source : sources) EXPECT_TRUE(service.GetPreview().GetWorld().IsAlive(service.GetPreview().FindAuthoring(source.id)));
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); ASSERT_TRUE(service.Tick(&scene, true, now));
    EXPECT_EQ(service.GetPreview().GetWorld().GetEntityCount(), 0u);
}

TEST(ECSEditorBaking, OpenTransactionsAreCapturedOnlyAfterCommitOrCancel)
{
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    auto& object = scene.CreateGameObject("before"); RegisterNames(service);
    const auto scope = Scope(); const auto now = SceneClock::now(); service.SetLive(true);
    ASSERT_TRUE(service.Tick(&scene, true, now)); const auto initial = service.GetLastResult().artifact;
    auto transaction = undo.BeginTransaction({"Rename transaction", "ECS Tests", {scope}});
    ASSERT_TRUE(transaction.result.Succeeded());
    ASSERT_TRUE(Hierarchy::RenameGameObject(undo, object, "after", scope).Succeeded());
    EXPECT_FALSE(service.Tick(&scene, true, now + 1s)); EXPECT_EQ(service.GetLastResult().artifact, initial);
    ASSERT_TRUE(undo.CommitTransaction(transaction.transaction).Succeeded());
    ASSERT_TRUE(service.Tick(&scene, true, now + 1s)); EXPECT_NE(service.GetLastResult().artifact, initial);
    const auto committed = service.GetLastResult().artifact;
    transaction = undo.BeginTransaction({"Canceled transaction", "ECS Tests", {scope}});
    ASSERT_TRUE(transaction.result.Succeeded());
    ASSERT_TRUE(Hierarchy::RenameGameObject(undo, object, "canceled", scope).Succeeded());
    EXPECT_FALSE(service.Tick(&scene, true, now + 2s));
    ASSERT_TRUE(undo.CancelTransaction(transaction.transaction).Succeeded());
    ASSERT_TRUE(service.Tick(&scene, true, now + 2s)); EXPECT_EQ(service.GetLastResult().artifact, committed);
}

TEST(ECSEditorBaking, DuplicateUndoRedoUsesFreshIdsAndKeepsThemOnRedo)
{
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    auto& original = scene.CreateGameObject("original");
    const auto originalId = CaptureSceneBakingSources(scene).at(0).id;
    ASSERT_TRUE(Hierarchy::DuplicateGameObject(undo, original, nullptr, Scope()).Succeeded());
    const auto sources = CaptureSceneBakingSources(scene); ASSERT_EQ(sources.size(), 2u);
    const auto duplicateId = sources[1].id; EXPECT_NE(originalId, duplicateId);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    ASSERT_EQ(CaptureSceneBakingSources(scene).size(), 1u);
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    EXPECT_EQ(CaptureSceneBakingSources(scene).at(1).id, duplicateId);
    const auto now = SceneClock::now(); service.SetLive(true); ASSERT_TRUE(service.Tick(&scene, true, now));
    EXPECT_TRUE(service.GetPreview().GetWorld().IsAlive(service.GetPreview().FindAuthoring(originalId)));
    EXPECT_TRUE(service.GetPreview().GetWorld().IsAlive(service.GetPreview().FindAuthoring(duplicateId)));
}

TEST(ECSEditorBaking, FailedBakingRetainsThePublishedArtifactAndPreviewThenRecovers)
{
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    auto& object = scene.CreateGameObject("valid"); RegisterNames(service);
    const auto now = SceneClock::now(); service.SetLive(true); ASSERT_TRUE(service.Tick(&scene, true, now));
    const auto previous = service.GetLastResult().artifact;
    const auto id = CaptureSceneBakingSources(scene).at(0).id;
    const auto entity = service.GetPreview().FindAuthoring(id);
    ASSERT_TRUE(Hierarchy::RenameGameObject(undo, object, "invalid", Scope()).Succeeded());
    ASSERT_TRUE(service.Tick(&scene, true, now)); EXPECT_FALSE(service.GetLastResult().succeeded);
    EXPECT_EQ(service.GetLastResult().artifact, previous); EXPECT_FALSE(service.GetLastResult().diagnostics.empty());
    EXPECT_EQ(service.GetPreview().FindAuthoring(id), entity); EXPECT_TRUE(service.GetPreview().GetWorld().IsAlive(entity));
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); ASSERT_TRUE(service.Tick(&scene, true, now));
    EXPECT_TRUE(service.GetLastResult().succeeded); EXPECT_EQ(service.GetLastResult().artifact, previous);
}

TEST(ECSEditorBaking, PlayAndFrozenUndoBlockCaptureAndSceneReplacementDropsStalePreview)
{
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    scene.CreateGameObject("edit"); const auto now = SceneClock::now(); service.SetLive(true);
    ASSERT_TRUE(service.Tick(&scene, true, now)); const auto previous = service.GetLastResult().artifact;
    const auto count = service.GetBakeCount();
    Scene play; play.CreateGameObject("play-a"); play.CreateGameObject("play-b");
    EXPECT_FALSE(service.Tick(&play, false, now + 1s)); EXPECT_FALSE(service.BakeNow());
    EXPECT_EQ(service.GetBakeCount(), count); EXPECT_EQ(service.GetLastResult().artifact, previous);
    EXPECT_EQ(service.GetPreview().GetWorld().GetEntityCount(), 1u);
    undo.Freeze(Undo::UndoFreezeReason::PlayMode);
    EXPECT_FALSE(service.Tick(&scene, true, now + 1s));
    undo.Unfreeze(Undo::UndoFreezeReason::PlayMode);
    ASSERT_TRUE(service.Tick(&scene, true, now + 1s)); EXPECT_TRUE(service.GetLastResult().succeeded);
    ASSERT_TRUE(service.Tick(&play, true, now + 2s)); EXPECT_EQ(service.GetPreview().GetWorld().GetEntityCount(), 2u);
    EXPECT_NE(service.GetLastResult().artifact, previous);
    EXPECT_FALSE(service.Tick(nullptr, true, now + 3s)); EXPECT_EQ(service.GetPreview().GetWorld().GetEntityCount(), 0u);
}

TEST(ECSEditorBaking, ExportWritesARealECSSceneAndImportsStableNativeAssetMetadata)
{
    ExportFixture fixture;
    const auto assets = fixture.root / "Assets";
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    scene.CreateGameObject("export").GetTransform()->SetLocalPosition({7, 0, 0});
    service.Tick(&scene, true);
    const auto output = assets / "Baked.ecsscene";
    NLS::Editor::Assets::AssetDatabaseFacade database(std::vector<std::filesystem::path>{fixture.root});
    auto import = [&](const auto& path)
    {
        std::string error;
        if (!NLS::Editor::ECSAuthoring::ImportBakedSceneAsset(database, path, error)) throw std::runtime_error(error);
        return true;
    };
    ASSERT_TRUE(service.ExportScene(output, import)) << testing::PrintToString(service.GetLastResult().diagnostics);
    const auto meta = Core::Assets::AssetMeta::Load(Core::Assets::GetAssetMetaPath(output));
    ASSERT_TRUE(meta); EXPECT_TRUE(meta->id.IsValid());
    const auto document = WorldArchive::ReadFile(output); ASSERT_EQ(document.entities.size(), 1u);
    EXPECT_EQ(document.kind, DocumentKind::Scene);
    EXPECT_EQ(document.entities[0].authoringId, CaptureSceneBakingSources(scene).at(0).id.GetGuid().ToString());
    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Baked.ecsscene");
    ASSERT_TRUE(manifest); EXPECT_NE(manifest->FindPrimaryArtifact(), nullptr);
    ASSERT_TRUE(service.ExportScene(output, import));
    EXPECT_EQ(Core::Assets::AssetMeta::Load(Core::Assets::GetAssetMetaPath(output))->id, meta->id);
    EXPECT_FALSE(service.ExportScene(assets / "authoring.scene"));
}

TEST(ECSEditorBaking, ActualPlaySandboxAndUndoDomainPreserveTheEditPreviewAcrossStop)
{
    Undo::EditorUndoService undo;
    Engine::SceneSystem::SceneManager manager;
    manager.LoadEmptyScene();
    auto* edit = manager.GetCurrentScene(); ASSERT_NE(edit, nullptr);
    auto& object = edit->CreateGameObject("edit"); object.GetTransform()->SetLocalPosition({3, 0, 0});
    const auto id = CaptureSceneBakingSources(*edit).at(0).id;
    SceneBakingService service(undo); service.SetLive(true);
    const auto now = SceneClock::now(); ASSERT_TRUE(service.Tick(edit, true, now));
    const auto entity = service.GetPreview().FindAuthoring(id);
    std::string error; ASSERT_TRUE(manager.BeginPlaySandbox(&error)) << error;
    ASSERT_TRUE(undo.BeginPlayDomain(17).Succeeded());
    auto* play = manager.GetCurrentScene(); ASSERT_NE(play, edit);
    play->GetGameObjects().front()->GetTransform()->SetLocalPosition({99, 0, 0});
    EXPECT_FALSE(service.Tick(play, true, now + 1s));
    EXPECT_FALSE(service.BakeNow()); EXPECT_FLOAT_EQ(X(service, id), 3);
    EXPECT_EQ(service.GetPreview().FindAuthoring(id), entity);
    ASSERT_TRUE(manager.DiscardPlaySandbox(&error)) << error;
    ASSERT_TRUE(undo.EndPlayDomain().Succeeded());
    EXPECT_EQ(manager.GetCurrentScene(), edit);
    ASSERT_TRUE(service.Tick(edit, true, now + 2s)); ASSERT_TRUE(service.GetLastResult().succeeded);
    EXPECT_FLOAT_EQ(X(service, id), 3); EXPECT_EQ(service.GetPreview().FindAuthoring(id), entity);
}

TEST(ECSEditorBaking, ResourceRevisionsRefreshLivePreviewAndMissingDependenciesKeepItsPublishedState)
{
    Undo::EditorUndoService undo; Scene scene; SceneBakingService service(undo);
    scene.CreateGameObject("resource");
    auto& schemas = service.GetPreview().GetWorld();
    schemas.RegisterComponent<BakedResource>("ecs-editor-test.Resource");
    RegisterComponentCodec<BakedResource>(schemas.GetRegistry(),
        [](const BakedResource& resource, const auto& context) { return ComponentJson{{"guid", context.Resource(resource.guid)}}; },
        [](BakedResource& resource, const ComponentJson& json, const auto& context)
        { resource.guid = context.Resource(json.at("guid").get<std::string>()); });
    const auto guid = Guid::New(); std::optional<std::string> revision = "one";
    service.SetResourceRevisionProvider([&](const Guid& id) { return id == guid ? revision : std::nullopt; });
    service.GetPipeline().RegisterBaker({"ecs-editor-test.Resource", 1,
        [guid](const Engine::GameObject&, const BakingCaptureContext& context) -> std::optional<ComponentJson>
        { return ComponentJson{{"guid", context.DependsOnResource(guid)}}; },
        [](const ComponentJson& input, BakingOutput& output) { output.AddComponent<BakedResource>(output.Primary(), input); }});
    const auto now = SceneClock::now(); service.SetLive(true);
    ASSERT_TRUE(service.Tick(&scene, true, now)); ASSERT_TRUE(service.GetLastResult().succeeded);
    EXPECT_EQ(service.GetLastResult().artifact->resources.at(0).hashOrVersion, "one");
    revision = "two";
    ASSERT_TRUE(service.Tick(&scene, true, now + 250ms)); ASSERT_TRUE(service.GetLastResult().succeeded);
    EXPECT_EQ(service.GetLastResult().artifact->resources.at(0).hashOrVersion, "two");
    const auto artifact = service.GetLastResult().artifact;
    const auto id = CaptureSceneBakingSources(scene).at(0).id;
    const auto entity = service.GetPreview().FindAuthoring(id);
    revision.reset(); ASSERT_TRUE(service.Tick(&scene, true, now + 500ms));
    EXPECT_FALSE(service.GetLastResult().succeeded); EXPECT_EQ(service.GetLastResult().artifact, artifact);
    EXPECT_EQ(service.GetPreview().FindAuthoring(id), entity); EXPECT_TRUE(service.GetPreview().GetWorld().IsAlive(entity));
}
}
