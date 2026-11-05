#include <Engine/ECS/Baking.h>
#include <Engine/ECS/TransformHierarchy.h>
#include <Engine/GameObject.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Components/TransformComponent.h>
#include <Engine/Components/CameraComponent.h>
#include <gtest/gtest.h>
#include <future>

namespace
{
using namespace NLS::ECS;
using namespace NLS::Engine::ECSIntegration;
using NLS::Engine::GameObject;
using NLS::Engine::SceneSystem::Scene;
using NLS::Engine::Serialize::ObjectId;
using NLS::Engine::Components::CameraComponent;
struct Marker { std::string name; Entity owner; std::string resource; };
void Schemas(World& world)
{
    RegisterTransformComponents(world); world.RegisterComponent<Marker>("bake.Marker");
    RegisterComponentCodec<Marker>(world.GetRegistry(),
        [](const Marker& value, const auto& context) { return ComponentJson{{"name", value.name}, {"owner", context.EntityId(value.owner)},
            {"resource", context.Resource(value.resource)}}; },
        [](Marker& value, const ComponentJson& json, const auto& context)
        { value = {json.at("name").get<std::string>(), context.Resolve(json.at("owner").get<std::string>()), context.Resource(json.at("resource").get<std::string>())}; });
}
BakingSource Source(GameObject& object, const std::string& key)
{ return {ObjectId{NLS::Guid::NewDeterministic("ecs-authoring-" + key)}, object.GetInstanceID()}; }
BakerDefinition Extras(size_t& runs)
{
    return {"Test.CameraExtras", 1,
        [](const GameObject& object, const BakingCaptureContext&) -> std::optional<ComponentJson>
        { if (!object.GetComponent<CameraComponent>()) return std::nullopt; return ComponentJson{{"name", object.GetName()}}; },
        [&runs](const ComponentJson& input, BakingOutput& output)
        {
            ++runs;
            if (input.at("name") == "invalid") throw std::invalid_argument("Rejected test authoring input");
            output.AddComponent<Marker>(output.Primary(), {{"name", input.at("name")}, {"owner", output.Primary()}, {"resource", ""}});
            const auto extra = output.CreateAdditionalEntity("camera-companion");
            output.AddComponent<Marker>(extra, {{"name", "extra"}, {"owner", output.Primary()}, {"resource", ""}});
        }};
}
TEST(ECSBaking, FullBakingUsesPersistentIdentityAndProducesAStandaloneLoadableArtifact)
{
    World schemas; Schemas(schemas); BakingPipeline pipeline(schemas.GetRegistryLifetime()); RegisterTransformBaker(pipeline);
    WorldDocument saved; ObjectId childId; std::string rootKey;
    {
        Scene scene; auto& root = scene.CreateGameObject("root"); auto& child = scene.CreateGameObject("child");
        root.GetTransform()->SetLocalPosition({7, 0, 0}); child.SetParent(root); child.GetTransform()->SetLocalPosition({4, 0, 0});
        const std::array sources{Source(root, "root"), Source(child, "child")}; childId = sources[1].id; rootKey = sources[0].id.GetGuid().ToString();
        const auto result = pipeline.Bake(sources, {.mode = BakingMode::Full, .kind = DocumentKind::Prefab});
        ASSERT_TRUE(result.succeeded); ASSERT_TRUE(result.artifact); EXPECT_EQ(result.statistics.rebuilt, 2u);
        saved = WorldArchive::Parse(WorldArchive::Encode(*result.artifact));
    }
    World runtime; Schemas(runtime); RegisterTransformSystem(runtime);
    const auto instance = WorldArchive::InstantiatePrefab(runtime, saved); runtime.Systems().RunPhase(SystemPhase::LateUpdate, {});
    const auto child = instance.byAuthoringId.at(childId.GetGuid().ToString());
    EXPECT_FLOAT_EQ(runtime.ReadComponent<WorldTransform>(child).matrix[3], 11);
    EXPECT_EQ(runtime.ReadComponent<Parent>(child).entity, instance.Find(rootKey));
}
TEST(ECSBaking, IncrementalCacheSkipsUnchangedBakersAndPreviewKeepsStableHandles)
{
    World schemas; Schemas(schemas); BakingPipeline pipeline(schemas.GetRegistryLifetime()); RegisterTransformBaker(pipeline);
    Scene scene; auto& object = scene.CreateGameObject("camera"); object.AddComponent<CameraComponent>();
    const std::array sources{Source(object, "camera")}; size_t runs = 0; pipeline.RegisterBaker(Extras(runs));
    const auto first = pipeline.Bake(sources); ASSERT_TRUE(first.succeeded); EXPECT_EQ(first.statistics.entities, 2u);
    BakingPreview preview(schemas.GetRegistryLifetime()); RegisterTransformSystem(preview.GetWorld()); ASSERT_TRUE(preview.Apply(first));
    preview.GetWorld().Systems().RunPhase(SystemPhase::LateUpdate, {}); const auto original = preview.FindAuthoring(sources[0].id);
    const auto same = pipeline.Bake(sources); ASSERT_TRUE(same.succeeded); EXPECT_EQ(same.statistics.rebuilt, 0u); EXPECT_EQ(same.statistics.reused, 2u);
    EXPECT_EQ(runs, 1u); EXPECT_EQ(first.artifact, same.artifact); EXPECT_FALSE(preview.Apply(same)); EXPECT_EQ(preview.FindAuthoring(sources[0].id), original);
    object.GetTransform()->SetLocalPosition({8, 0, 0}); const auto changed = pipeline.Bake(sources);
    ASSERT_TRUE(changed.succeeded); EXPECT_EQ(changed.statistics.rebuilt, 1u); EXPECT_EQ(changed.statistics.reused, 1u);
    EXPECT_TRUE(preview.Apply(changed)); EXPECT_FALSE(preview.GetWorld().IsAlive(original));
    preview.GetWorld().Systems().RunPhase(SystemPhase::LateUpdate, {});
    EXPECT_FLOAT_EQ(preview.GetWorld().ReadComponent<WorldTransform>(preview.FindAuthoring(sources[0].id)).matrix[3], 8);
    EXPECT_EQ(runs, 1u);
}
TEST(ECSBaking, ComponentAndObjectRemovalCleanTheirComponentsAndAdditionalEntities)
{
    World schemas; Schemas(schemas); BakingPipeline pipeline(schemas.GetRegistryLifetime()); RegisterTransformBaker(pipeline);
    Scene scene; auto& object = scene.CreateGameObject("camera"); auto* camera = object.AddComponent<CameraComponent>();
    const std::array sources{Source(object, "removed")}; size_t runs = 0; pipeline.RegisterBaker(Extras(runs));
    auto result = pipeline.Bake(sources); ASSERT_TRUE(result.succeeded); EXPECT_EQ(result.statistics.entities, 2u);
    ASSERT_TRUE(object.RemoveComponent(camera)); result = pipeline.Bake(sources); ASSERT_TRUE(result.succeeded);
    EXPECT_EQ(result.statistics.removed, 1u); ASSERT_EQ(result.artifact->entities.size(), 1u);
    for (const auto& component : result.artifact->entities[0].components) EXPECT_NE(component.name, "bake.Marker");
    scene.DestroyGameObject(object); scene.CollectGarbages(); result = pipeline.Bake(sources);
    ASSERT_TRUE(result.succeeded); EXPECT_TRUE(result.artifact->entities.empty()); EXPECT_EQ(result.statistics.removed, 1u);
}
TEST(ECSBaking, ResourceRevisionsInvalidateCacheAndMissingResourcesPreservePreviousArtifact)
{
    World schemas; Schemas(schemas); BakingPipeline pipeline(schemas.GetRegistryLifetime());
    Scene scene; auto& object = scene.CreateGameObject("asset"); const std::array sources{Source(object, "asset")};
    const auto guid = NLS::Guid::NewDeterministic("ecs-resource"); size_t runs = 0;
    pipeline.RegisterBaker({"Test.Resource", 1,
        [guid](const GameObject&, const BakingCaptureContext& context) -> std::optional<ComponentJson> { return ComponentJson{{"guid", context.DependsOnResource(guid)}}; },
        [&runs](const ComponentJson& input, BakingOutput& output)
        { ++runs; output.AddComponent<Marker>(output.Primary(), {{"name", "resource"}, {"owner", output.Primary()}, {"resource", input.at("guid")}}); }});
    std::optional<std::string> revision = "v1"; BakingOptions options;
    options.resourceRevision = [&](const NLS::Guid& id) { return id == guid ? revision : std::nullopt; };
    auto first = pipeline.Bake(sources, options); ASSERT_TRUE(first.succeeded); EXPECT_EQ(runs, 1u);
    EXPECT_TRUE(pipeline.Bake(sources, options).succeeded); EXPECT_EQ(runs, 1u);
    revision = "v2"; const auto changed = pipeline.Bake(sources, options); ASSERT_TRUE(changed.succeeded); EXPECT_EQ(runs, 2u);
    EXPECT_EQ(changed.artifact->resources[0].hashOrVersion, "v2");
    revision.reset(); const auto failure = pipeline.Bake(sources, options); EXPECT_FALSE(failure.succeeded);
    EXPECT_EQ(failure.artifact, changed.artifact); EXPECT_FALSE(failure.diagnostics.empty());
}
TEST(ECSBaking, FailedBakePreservesCacheAndPreviewAndPlayWorldsAreIndependent)
{
    World schemas; Schemas(schemas); BakingPipeline pipeline(schemas.GetRegistryLifetime()); RegisterTransformBaker(pipeline);
    Scene scene; auto& object = scene.CreateGameObject("valid"); object.AddComponent<CameraComponent>();
    const std::array sources{Source(object, "failure")}; size_t runs = 0; pipeline.RegisterBaker(Extras(runs));
    const auto first = pipeline.Bake(sources); ASSERT_TRUE(first.succeeded);
    BakingPreview preview(schemas.GetRegistryLifetime()); ASSERT_TRUE(preview.Apply(first)); const auto previous = preview.FindAuthoring(sources[0].id);
    World play({}, schemas.GetRegistryLifetime()); const auto playInstance = WorldArchive::Instantiate(play, *first.artifact);
    object.SetName("invalid"); auto failure = pipeline.Bake(sources); EXPECT_FALSE(failure.succeeded); EXPECT_EQ(failure.artifact, first.artifact);
    EXPECT_FALSE(preview.Apply(failure)); EXPECT_TRUE(preview.GetWorld().IsAlive(previous)); EXPECT_EQ(play.GetEntityCount(), 2u);
    object.SetName("valid"); const auto recovered = pipeline.Bake(sources); ASSERT_TRUE(recovered.succeeded); EXPECT_EQ(recovered.statistics.rebuilt, 0u);
    EXPECT_EQ(recovered.artifact, first.artifact); EXPECT_TRUE(play.IsAlive(playInstance.entities[0]));
}
TEST(ECSBaking, DuplicateComponentOwnersAndInvalidBakerVersionsAreDiagnosed)
{
    World schemas; Schemas(schemas); BakingPipeline pipeline(schemas.GetRegistryLifetime()); RegisterTransformBaker(pipeline);
    Scene scene; auto& object = scene.CreateGameObject("object"); const std::array sources{Source(object, "conflict")};
    const auto first = pipeline.Bake(sources); ASSERT_TRUE(first.succeeded);
    BakerDefinition duplicate{"Test.Conflict", 1,
        [](const GameObject&, const auto&) -> std::optional<ComponentJson> { return ComponentJson::object(); },
        [](const ComponentJson&, BakingOutput& output) { output.AddComponent<LocalTransform>(output.Primary(), {{"position", {0,0,0}}, {"rotation", {0,0,0,1}}, {"scale", {1,1,1}}}); }};
    pipeline.RegisterBaker(duplicate); const auto failure = pipeline.Bake(sources); EXPECT_FALSE(failure.succeeded); EXPECT_EQ(failure.artifact, first.artifact);
    EXPECT_THROW(pipeline.ReplaceBaker(duplicate), std::invalid_argument); duplicate.version = 2; EXPECT_NO_THROW(pipeline.ReplaceBaker(duplicate));
    EXPECT_TRUE(pipeline.RemoveBaker(duplicate.id)); EXPECT_TRUE(pipeline.Bake(sources).succeeded);
}
TEST(ECSBaking, PersistentIdsSurviveRenamesAndReorderedInputAndWorkersCannotReadGameObjects)
{
    World schemas; Schemas(schemas); BakingPipeline pipeline(schemas.GetRegistryLifetime()); RegisterTransformBaker(pipeline);
    Scene scene; auto& first = scene.CreateGameObject("first"); auto& second = scene.CreateGameObject("second");
    std::array sources{Source(first, "stable-first"), Source(second, "stable-second")};
    const auto original = pipeline.Bake(sources); ASSERT_TRUE(original.succeeded); first.SetName("renamed"); std::swap(sources[0], sources[1]);
    const auto same = pipeline.Bake(sources); ASSERT_TRUE(same.succeeded); EXPECT_EQ(same.artifact, original.artifact); EXPECT_EQ(same.statistics.rebuilt, 0u);
    auto worker = std::async(std::launch::async, [&] { EXPECT_THROW(pipeline.Bake(sources), std::logic_error); }); worker.get();
}
TEST(ECSBaking, MissingCachedReferencesTryFullFallbackAndKeepPreviousArtifactOnFailure)
{
    World schemas; Schemas(schemas); BakingPipeline pipeline(schemas.GetRegistryLifetime());
    Scene scene; auto& first = scene.CreateGameObject("a"); auto& second = scene.CreateGameObject("b");
    std::array sources{Source(first, "a"), Source(second, "b")}; const auto secondId = sources[1].id.GetGuid().ToString();
    pipeline.RegisterBaker({"Test.Reference", 1,
        [secondId](const GameObject&, const auto&) -> std::optional<ComponentJson> { return ComponentJson{{"target", secondId}}; },
        [](const ComponentJson& input, BakingOutput& output) { output.AddComponent<Marker>(output.Primary(), {{"name", "reference"}, {"owner", input.at("target")}, {"resource", ""}}); }});
    const auto firstResult = pipeline.Bake(sources); ASSERT_TRUE(firstResult.succeeded);
    const auto failure = pipeline.Bake(std::span(sources).first(1)); EXPECT_FALSE(failure.succeeded); EXPECT_TRUE(failure.statistics.fullFallback);
    EXPECT_EQ(failure.artifact, firstResult.artifact); EXPECT_TRUE(pipeline.Bake(sources).succeeded);
}
}
