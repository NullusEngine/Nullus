#include <gtest/gtest.h>
#include <Engine/ECS/RuntimeWorld.h>
#include <Engine/ECS/TransformHierarchy.h>
#include "JobTestSupport.h"
#include <Base/Jobs/JobSafety.h>
#include <chrono>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Components/TransformComponent.h>
#include <Engine/Components/RigidbodyComponent.h>
#include <Engine/Components/Rigidbody2DComponent.h>
#include <Scripting/RuntimePlaySession.h>
#include <Scripting/ScriptEngineApi.h>
#include <Scripting/LuaScriptBackend.h>
#include <Scripting/ScriptComponent.h>
#include <atomic>
#include <thread>

using namespace NLS::ECS;
using namespace NLS::Engine;
using namespace NLS::Engine::ECSIntegration;
namespace
{
class HybridMovement final : public System
{
public:
    explicit HybridMovement(int& updates) : m_updates(updates) {}
    void OnUpdate(SystemContext& context) override
    {
        ++m_updates;
        context.world.Query<Write<WorldPose>>().Each([&](Entity, WorldPose& pose)
        { pose.position[0] += static_cast<float>(context.time.deltaTime) * 10; });
    }
private:
    int& m_updates;
};
class PhaseRecorder final : public System
{
public:
    explicit PhaseRecorder(std::vector<SystemPhase>& phases) : m_phases(phases) {}
    void OnUpdate(SystemContext& context) override { m_phases.push_back(context.phase); }
private:
    std::vector<SystemPhase>& m_phases;
};
class StartupTime final : public System
{
public:
    explicit StartupTime(SystemTime& value) : output(value) {}
    void OnUpdate(SystemContext& context) override { output = context.time; }
private:
    SystemTime& output;
};
struct StreamFile
{
    std::filesystem::path path;
    StreamFile()
    {
        path = std::filesystem::path("tmp") / ("ecs-hybrid-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        World source; RegisterTransformComponents(source); const auto entity = source.CreateEntity(); AddTransform(source, entity, {{7, 0, 0}});
        WorldArchive::WriteFile(path, WorldArchive::CaptureAll(source));
    }
    ~StreamFile() { std::error_code error; std::filesystem::remove(path, error); }
};
}
TEST(ECSHybrid, RuntimePhasePumpsReadyChunksWithoutWaitingAndStopUnloadsThem)
{
    ECSTest::JobSystemScope jobs; StreamFile file; RuntimeWorld runtime; RegisterTransformSystem(runtime.GetWorld());
    const auto request = runtime.Streaming().RequestLoad(10, file.path);
    NLS::Base::Jobs::ShutdownJobSystem(); // Parsing finishes; World still has no entities.
    EXPECT_EQ(runtime.GetWorld().GetEntityCount(), 0u);
    {
        NLS::Base::Jobs::DisallowJobSyncWaitScope disallow;
        runtime.RunPhase(SystemPhase::Update, {}); // Pump performs no Jobs wait.
    }
    EXPECT_EQ(runtime.Streaming().GetInfo(request).status, SceneLoadStatus::Loaded);
    const auto entity = runtime.Streaming().GetEntities(10).at(0);
    runtime.RunPhase(SystemPhase::LateUpdate, {}); EXPECT_FLOAT_EQ(runtime.GetWorld().ReadComponent<WorldTransform>(entity).matrix[3], 7);
    runtime.Stop(); EXPECT_TRUE(runtime.GetWorld().IsClosed()); EXPECT_FALSE(runtime.GetWorld().IsAlive(entity));
}
TEST(ECSHybrid, LegacySceneReplacementUnloadsStreamedChunks)
{
    ECSTest::JobSystemScope jobs; StreamFile file; SceneSystem::Scene first, second; RuntimeWorld runtime;
    RegisterTransformComponents(runtime.GetWorld()); runtime.AttachScene(first);
    ASSERT_EQ(runtime.Streaming().Wait(runtime.Streaming().RequestLoad(1, file.path)).status, SceneLoadStatus::Loaded);
    const auto old = runtime.Streaming().GetEntities(1).at(0); runtime.AttachScene(second);
    EXPECT_FALSE(runtime.GetWorld().IsAlive(old)); EXPECT_TRUE(runtime.Streaming().GetEntities(1).empty());
}
TEST(ECSHybrid, LegacyObjectsDoNotAutomaticallyCreateEntities)
{
    SceneSystem::Scene scene; RuntimeWorld runtime; runtime.AttachScene(scene);
    auto& legacy = scene.CreateGameObject("legacy"); legacy.GetTransform()->SetLocalPosition({3, 0, 0});
    EXPECT_EQ(runtime.GetWorld().GetEntityCount(), 0u);
    auto entity = runtime.GetWorld().CreateEntity();
    EXPECT_EQ(scene.GetGameObjects().size(), 1u); EXPECT_EQ(runtime.GetWorld().GetEntityCount(), 1u);
    EXPECT_EQ(runtime.ResolveGameObject(entity), nullptr); EXPECT_EQ(legacy.GetTransform()->GetWorldPosition().x, 3);
}
TEST(ECSHybrid, EntityAuthorityWritesOneDirectionAndRevalidatesHandles)
{
    SceneSystem::Scene scene; RuntimeWorld runtime; runtime.AttachScene(scene);
    auto& object = scene.CreateGameObject("presentation"); object.GetTransform()->SetWorldPosition({1, 0, 0});
    auto entity = runtime.GetWorld().CreateEntity(); WorldPose pose; pose.position[0] = 9;
    runtime.GetWorld().AddComponent<WorldPose>(entity, pose);
    HybridLinkOptions options; options.authority = TransformAuthority::Entity; options.authoringId = "source-guid:1";
    runtime.Link(entity, object, options);
    runtime.SyncFromGameObjects(); EXPECT_EQ(runtime.GetWorld().ReadComponent<WorldPose>(entity).position[0], 9);
    runtime.SyncToGameObjects(); EXPECT_EQ(object.GetTransform()->GetWorldPosition().x, 9);
    object.GetTransform()->SetWorldPosition({18, 0, 0}); runtime.SyncToGameObjects();
    EXPECT_EQ(object.GetTransform()->GetWorldPosition().x, 9);
    EXPECT_EQ(runtime.FindAuthoring("source-guid:1"), entity); EXPECT_EQ(runtime.FindEntity(object.GetInstanceID()), entity);
    runtime.GetWorld().DestroyEntity(entity); runtime.Prune();
    EXPECT_TRUE(object.IsAlive()); EXPECT_EQ(runtime.ResolveGameObject(entity), nullptr);
}
TEST(ECSHybrid, GameObjectAuthorityNeverReceivesECSWrites)
{
    SceneSystem::Scene scene; RuntimeWorld runtime;
    auto& object = scene.CreateGameObject("authoring"); object.GetTransform()->SetWorldPosition({2, 0, 0});
    auto entity = runtime.GetWorld().CreateEntity(); HybridLinkOptions options; options.authority = TransformAuthority::GameObject;
    runtime.Link(entity, object, options);
    auto pose = runtime.GetWorld().ReadComponent<WorldPose>(entity); pose.position[0] = 99;
    runtime.GetWorld().SetComponent<WorldPose>(entity, pose); runtime.SyncToGameObjects();
    EXPECT_EQ(object.GetTransform()->GetWorldPosition().x, 2);
    runtime.SyncFromGameObjects(); EXPECT_EQ(runtime.GetWorld().ReadComponent<WorldPose>(entity).position[0], 2);
    object.GetTransform()->SetWorldPosition({7, 0, 0}); runtime.SyncFromGameObjects();
    EXPECT_EQ(runtime.GetWorld().ReadComponent<WorldPose>(entity).position[0], 7);
    const auto cursor = runtime.GetWorld().GetChangeToken(); runtime.SyncFromGameObjects();
    EXPECT_EQ(runtime.GetWorld().Query<Read<WorldPose>>().Changed<WorldPose>(cursor).Count(), 0u);
}
TEST(ECSHybrid, DestroyedGameObjectsCleanEntitiesAtTheSafePoint)
{
    SceneSystem::Scene scene; RuntimeWorld runtime; runtime.AttachScene(scene);
    auto& object = scene.CreateGameObject("owned"); auto entity = runtime.GetWorld().CreateEntity();
    runtime.Link(entity, object); ASSERT_TRUE(scene.DestroyGameObject(object));
    EXPECT_TRUE(runtime.GetWorld().IsAlive(entity)); runtime.Prune(); EXPECT_FALSE(runtime.GetWorld().IsAlive(entity));
    auto& replacement = scene.CreateGameObject("replacement");
    EXPECT_FALSE(runtime.FindEntity(replacement.GetInstanceID()).IsValid());
}
TEST(ECSHybrid, EntityOwnedPresentationObjectsAreRemoved)
{
    SceneSystem::Scene scene; RuntimeWorld runtime; auto& object = scene.CreateGameObject("proxy");
    auto entity = runtime.GetWorld().CreateEntity(); HybridLinkOptions options; options.destroyObjectWithEntity = true;
    runtime.Link(entity, object, options); runtime.GetWorld().DestroyEntity(entity); runtime.Prune();
    EXPECT_TRUE(scene.GetGameObjects().empty()); EXPECT_TRUE(runtime.GetLinks().empty());
}
TEST(ECSHybrid, SceneReplacementKeepsUnownedEntitiesAndCleansOldSceneEntities)
{
    SceneSystem::Scene first, second; RuntimeWorld runtime; runtime.AttachScene(first);
    auto global = runtime.GetWorld().CreateEntity(), linked = runtime.GetWorld().CreateEntity();
    auto& object = first.CreateGameObject("first-scene"); runtime.Link(linked, object);
    runtime.AttachScene(second);
    EXPECT_FALSE(runtime.GetWorld().IsAlive(linked)); EXPECT_TRUE(runtime.GetWorld().IsAlive(global)); EXPECT_TRUE(object.IsAlive());
}
TEST(ECSHybrid, DynamicPhysicsAuthorityCannotBeOverwrittenByECS)
{
    SceneSystem::Scene scene; RuntimeWorld runtime; auto& object = scene.CreateGameObject("body");
    ASSERT_NE(object.AddComponent<Components::RigidbodyComponent>(), nullptr);
    auto entity = runtime.GetWorld().CreateEntity(); HybridLinkOptions options; options.authority = TransformAuthority::Entity;
    EXPECT_THROW(runtime.Link(entity, object, options), std::invalid_argument);
    options.authority = TransformAuthority::GameObject; EXPECT_NO_THROW(runtime.Link(entity, object, options));
}
TEST(ECSHybrid, BridgeRejectsWorkerThreadEngineAccess)
{
    RuntimeWorld runtime; std::atomic<bool> rejected{false};
    std::thread worker([&] { try { runtime.SyncToGameObjects(); } catch (const std::logic_error&) { rejected = true; } });
    worker.join(); EXPECT_TRUE(rejected.load());
}
TEST(ECSHybrid, Dynamic2DPhysicsAuthorityCannotBeOverwrittenByECS)
{
    SceneSystem::Scene scene; RuntimeWorld runtime; auto& object = scene.CreateGameObject("body2D");
    auto* body = object.AddComponent<Components::Rigidbody2DComponent>(); ASSERT_NE(body, nullptr);
    const auto entity = runtime.GetWorld().CreateEntity(); HybridLinkOptions options; options.authority = TransformAuthority::Entity;
    EXPECT_THROW(runtime.Link(entity, object, options), std::invalid_argument);
    body->SetMotionType(Components::Rigidbody2DComponent::EMotionType::KINEMATIC);
    EXPECT_NO_THROW(runtime.Link(entity, object, options));
}
TEST(ECSHybrid, ExistingPlaySessionRemainsOptional)
{
    NLS::Scripting::ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    SceneSystem::SceneManager scenes; NLS::Scripting::RuntimePlaySession session;
    ASSERT_TRUE(session.Begin(scripts, scenes, 8001).Succeeded()); scenes.GetCurrentScene()->Play();
    EXPECT_EQ(session.GetECS(), nullptr); session.Tick(0.02f); EXPECT_EQ(session.GetFrame().frameIndex, 1u);
    EXPECT_TRUE(session.End().Succeeded());
}
TEST(ECSHybrid, PlayPauseStepStopAndRestartDriveOneECSWorld)
{
    NLS::Scripting::ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    SceneSystem::SceneManager scenes; scenes.GetCurrentScene()->CreateGameObject("presentation");
    NLS::Scripting::RuntimePlaySession session; int updates = 0; Entity entity; std::vector<SystemPhase> phases;
    session.EnableECS([&](RuntimeWorld& runtime, SceneSystem::Scene& scene)
    {
        auto& world = runtime.GetWorld(); entity = world.CreateEntity();
        HybridLinkOptions options; options.authority = TransformAuthority::Entity;
        runtime.Link(entity, *scene.FindGameObjectByName("presentation"), options);
        SystemDescription movement{"movement"}; movement.Writes<WorldPose>(world);
        world.Systems().RegisterSystem<HybridMovement>(movement, updates);
        for (int phase = 0; phase < int(SystemPhase::Count); ++phase)
        { SystemDescription recorder{"phase" + std::to_string(phase)}; recorder.phase = static_cast<SystemPhase>(phase); world.Systems().RegisterSystem<PhaseRecorder>(recorder, phases); }
    });
    std::string error; ASSERT_TRUE(session.ConfigurePhysicsTiming({0.02, 8, 0.25}, error)) << error;
    ASSERT_TRUE(session.Begin(scripts, scenes, 8002).Succeeded()); scenes.GetCurrentScene()->Play();
    auto lifetime = session.GetECS()->GetWorld().GetLifetime(); const auto firstWorld = entity.world;
    session.Tick(0.04f); EXPECT_EQ(updates, 1);
    EXPECT_NEAR(scenes.GetCurrentScene()->FindGameObjectByName("presentation")->GetTransform()->GetWorldPosition().x, 0.4f, 1e-5);
    ASSERT_FALSE(phases.empty()); EXPECT_EQ(phases.front(), SystemPhase::Initialization);
    EXPECT_EQ(phases.back(), SystemPhase::Cleanup);
    session.SetPaused(true); session.Tick(1); EXPECT_EQ(updates, 1);
    session.Step(); EXPECT_EQ(updates, 2);
    EXPECT_NEAR(scenes.GetCurrentScene()->FindGameObjectByName("presentation")->GetTransform()->GetWorldPosition().x, 0.6f, 1e-5);
    ASSERT_TRUE(session.End().Succeeded()); EXPECT_TRUE(lifetime.expired()); EXPECT_EQ(session.GetECS(), nullptr);
    scenes.LoadEmptyScene(); scenes.GetCurrentScene()->CreateGameObject("presentation");
    ASSERT_TRUE(session.Begin(scripts, scenes, 8003).Succeeded()); EXPECT_NE(entity.world, firstWorld);
    EXPECT_TRUE(session.End().Succeeded());
}
TEST(ECSHybrid, InitializationUsesConfiguredTimingAndStepWorksAtZeroTimeScale)
{
    NLS::Scripting::ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    SceneSystem::SceneManager scenes; NLS::Scripting::RuntimePlaySession session; SystemTime startup;
    session.EnableECS([&](RuntimeWorld& runtime, SceneSystem::Scene&)
    {
        SystemDescription description{"startup"}; description.phase = SystemPhase::Initialization;
        runtime.GetWorld().Systems().RegisterSystem<StartupTime>(description, startup);
    });
    std::string error; ASSERT_TRUE(session.ConfigurePhysicsTiming({0.02, 8, 0.25}, error)) << error;
    ASSERT_TRUE(session.Begin(scripts, scenes, 8004).Succeeded()); scenes.GetCurrentScene()->Play();
    NLS::Scripting::ScriptEngineApi::SetTimeScale(0); session.Tick(0.1f);
    EXPECT_NEAR(startup.fixedDeltaTime, 0.02, 1e-6); EXPECT_DOUBLE_EQ(startup.timeScale, 0);
    EXPECT_DOUBLE_EQ(startup.deltaTime, 0); EXPECT_EQ(session.GetFrame().fixedFrameIndex, 0u);
    session.SetPaused(true); session.Step(); EXPECT_EQ(session.GetFrame().fixedFrameIndex, 1u);
    EXPECT_NEAR(session.GetFrame().time, 0.02, 1e-6); EXPECT_TRUE(session.End().Succeeded());
}
TEST(ECSHybrid, LegacyLuaAndECSBothUpdateOncePerSessionFrame)
{
#if NLS_HAS_LUA_VM
    using namespace NLS::Scripting;
    ScriptApiDatabase api; ScriptTypeDescriptor type; type.name = "ECSLegacyCounter"; type.id = MakeStableScriptId(type.name);
    type.fields.push_back({1, "updates", {0, ScriptValueKind::Int32, "int", 4, 4, true}}); ASSERT_TRUE(api.RegisterType(type));
    ScriptRuntime scripts; ASSERT_TRUE(scripts.RegisterBackend(std::make_unique<LuaScriptBackend>()).Succeeded());
    ASSERT_TRUE(scripts.Initialize(api).Succeeded()); SceneSystem::SceneManager scenes;
    auto& object = scenes.GetCurrentScene()->CreateGameObject("legacy-lua");
    auto* legacy = object.AddComponent<ScriptComponent>(); ASSERT_NE(legacy, nullptr);
    ScriptAsset asset; asset.assetId = NLS::Core::Assets::AssetId::New(); asset.language = ScriptLanguage::Lua;
    asset.scriptType = type.id; asset.sourcePath = "ECSLegacyCounter.lua";
    asset.sourceText = "return {updates=0, Update=function(self) self.updates=self.updates+1 end}";
    legacy->SetScriptAsset(asset); RuntimePlaySession session; int updates = 0;
    session.EnableECS([&](RuntimeWorld& runtime, SceneSystem::Scene&)
    {
        auto& world = runtime.GetWorld(); auto entity = world.CreateEntity(); world.AddComponent<WorldPose>(entity);
        SystemDescription description{"movement"}; description.Writes<WorldPose>(world);
        world.Systems().RegisterSystem<HybridMovement>(description, updates);
    });
    ASSERT_TRUE(session.Begin(scripts, scenes, 8005).Succeeded()); scenes.GetCurrentScene()->Play();
    for (int frame = 0; frame < 3; ++frame) session.Tick(0.02f);
    ScriptValue value; ASSERT_TRUE(legacy->GetRuntimeSerializedField(1, value));
    EXPECT_EQ(std::get<int32_t>(value), 3); EXPECT_EQ(updates, 3);
    EXPECT_TRUE(session.End().Succeeded());
#else
    GTEST_SKIP() << "Lua VM is disabled; legacy Lua coexistence was not exercised.";
#endif
}
