#include <gtest/gtest.h>
#include "Core/PlayModeController.h"
#include "Scripting/RuntimePlaySession.h"
#include "Scripting/ScriptEngineApi.h"
#include "Scripting/InputScriptApi.h"
#include <Input/InputRebinding.h>
#include <Scripting/ProjectPhysicsTiming.h>

TEST(ProjectPhysicsTimingTests, PartialProjectOverridesKeepSixtyHzAndExplicitSessionSettings)
{
    NLS::Filesystem::IniFile project("");
    NLS::Scripting::RuntimePlaySession session; std::string error;
    project.Add<int>("maximum_fixed_steps",3);
    ASSERT_TRUE(NLS::Scripting::ConfigureProjectPhysicsTiming(session,project,error)) << error;
    EXPECT_DOUBLE_EQ(session.GetPhysicsClock().GetSettings().fixedDeltaTime,1.0/60);
    ASSERT_TRUE(session.ConfigurePhysicsTiming({0.02,7,0.4},error));
    ASSERT_TRUE(NLS::Scripting::ConfigureProjectPhysicsTiming(session,project,error));
    EXPECT_DOUBLE_EQ(session.GetPhysicsClock().GetSettings().fixedDeltaTime,0.02);
    EXPECT_DOUBLE_EQ(session.GetPhysicsClock().GetSettings().maximumAccumulatedTime,0.4);
    EXPECT_EQ(session.GetPhysicsClock().GetSettings().maxSubsteps,3u);
}

TEST(ProjectPhysicsTimingTests, AccumulationOnlyOverrideIsAppliedAndInvalidValueIsAtomic)
{
    NLS::Filesystem::IniFile project("");
    NLS::Scripting::RuntimePlaySession session; std::string error;
    project.Add<double>("maximum_physics_accumulated_time",0.5);
    ASSERT_TRUE(NLS::Scripting::ConfigureProjectPhysicsTiming(session,project,error));
    EXPECT_DOUBLE_EQ(session.GetPhysicsClock().GetSettings().maximumAccumulatedTime,0.5);
    project.Set<double>("maximum_physics_accumulated_time",0.001);
    EXPECT_FALSE(NLS::Scripting::ConfigureProjectPhysicsTiming(session,project,error)); EXPECT_FALSE(error.empty());
    EXPECT_DOUBLE_EQ(session.GetPhysicsClock().GetSettings().maximumAccumulatedTime,0.5);
}
#include "Core/GameViewInputRouter.h"
#include "Undo/EditorUndoService.h"
#include "Engine/Audio/AudioDevice.h"
#include "Engine/Rendering/GameOutput.h"
#include "Physics/PhysicsWorld.h"
#include "Scripting/LuaScriptBackend.h"
#include "Scripting/FakeScriptBackend.h"
#include "Scripting/ScriptComponent.h"
#include "Scripting/PlayScriptBindings.h"
#include "Engine/Components/RigidbodyComponent.h"
#include "Engine/Components/TransformComponent.h"
#include "Scripting/ScriptApiManifest.h"
#include <fstream>
#include <filesystem>
#include <cmath>
#include <thread>
#include <ServiceLocator.h>

using namespace NLS::Editor::Core;
using namespace NLS::Scripting;

namespace
{
class PlayTestEdit final : public NLS::Editor::Undo::IEditorUndoCommand
{
public:
    PlayTestEdit(std::vector<NLS::Editor::Undo::UndoScopeId> scopes, int& value, int before, int after)
        : m_value(value), m_before(before), m_after(after) { m_info.label="Play test edit"; m_info.scopes=std::move(scopes); }
    NLS::Editor::Undo::UndoCommandInfo GetInfo() const override { return m_info; }
    NLS::Editor::Undo::UndoValidationResult Validate(NLS::Editor::Undo::UndoDirection, const NLS::Editor::Undo::UndoExecutionContext&) const override
    { return NLS::Editor::Undo::UndoValidationResult::Ready(); }
    NLS::Editor::Undo::UndoStartResult Apply(NLS::Editor::Undo::UndoDirection direction,
        const NLS::Editor::Undo::UndoExecutionContext&, NLS::Editor::Undo::UndoCompletion) override
    { m_value=direction==NLS::Editor::Undo::UndoDirection::Undo?m_before:m_after; return NLS::Editor::Undo::UndoStartResult::Completed(); }
    bool TryMergeFrom(const IEditorUndoCommand&) override { return false; }
private:
    NLS::Editor::Undo::UndoCommandInfo m_info;
    int& m_value; int m_before, m_after;
};
}

TEST(PlayUndoTests, RestoresEditRedoAndDiscardsRuntimeHistory)
{
    using namespace NLS::Editor::Undo;
    EditorUndoService undo; int edit=0, runtime=0;
    const UndoScopeId original{UndoScopeKind::Scene,"edit-scene",1};
    ASSERT_TRUE(undo.Execute(std::make_unique<PlayTestEdit>(std::vector{original},edit,0,1)).Succeeded());
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    ASSERT_TRUE(undo.BeginPlayDomain(7).Succeeded());
    ASSERT_TRUE(undo.Execute(std::make_unique<PlayTestEdit>(std::vector{UndoScopeId{UndoScopeKind::Scene,"play-scene:7",7}},runtime,0,1)).Succeeded());
    EXPECT_FALSE(undo.Execute(std::make_unique<PlayTestEdit>(std::vector{original},edit,0,9)).Succeeded());
    ASSERT_TRUE(undo.EndPlayDomain().Succeeded());
    EXPECT_TRUE(undo.CanRedo()); EXPECT_FALSE(undo.CanUndo());
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); EXPECT_EQ(edit,1);
    EXPECT_EQ(undo.GetTransactionCount(),1u);
}
TEST(PlayUndoTests, KeepsExplicitAssetEditsAndRejectsMixedLifetimes)
{
    using namespace NLS::Editor::Undo;
    EditorUndoService undo; int edit=0, asset=0, runtime=0;
    const UndoScopeId original{UndoScopeKind::Scene,"edit-scene",1};
    const UndoScopeId play{UndoScopeKind::Scene,"play-scene:8",8};
    const UndoScopeId persistent{UndoScopeKind::Asset,"asset:abc",1};
    ASSERT_TRUE(undo.Execute(std::make_unique<PlayTestEdit>(std::vector{original},edit,0,1)).Succeeded());
    undo.MarkSaved(original);
    ASSERT_TRUE(undo.BeginPlayDomain(8).Succeeded());
    EXPECT_FALSE(undo.Execute(std::make_unique<PlayTestEdit>(std::vector{play,persistent},asset,0,99)).Succeeded());
    ASSERT_TRUE(undo.Execute(std::make_unique<PlayTestEdit>(std::vector{play},runtime,0,2)).Succeeded());
    ASSERT_TRUE(undo.Execute(std::make_unique<PlayTestEdit>(std::vector{persistent},asset,0,3)).Succeeded());
    undo.MarkSaved(persistent);
    ASSERT_TRUE(undo.EndPlayDomain().Succeeded());
    EXPECT_FALSE(undo.GetScopeState(original).IsDirty());
    EXPECT_FALSE(undo.GetScopeState(persistent).IsDirty());
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_EQ(asset,0); EXPECT_EQ(edit,1);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_EQ(edit,0);
    EXPECT_FALSE(undo.CanUndo());
}

TEST(GameViewPresentationTests, ResolutionDpiAndZoomAreIndependent)
{
    const auto view=GameViewPresentation::Resolve({100,100},{800,600},GameViewSizeMode::FixedResolution,1920,1080,2,false,true,1,{});
    EXPECT_EQ(view.width,1920u);EXPECT_EQ(view.height,1080u);EXPECT_FLOAT_EQ(view.imageMin.y,175);
    const auto pixel=view.ToOutput({500,400});EXPECT_NEAR(pixel.x,960,0.01);EXPECT_NEAR(pixel.y,540,0.01);
    EXPECT_FALSE(view.Contains({500,120}));
    const auto dpi=GameViewPresentation::Resolve({},{800,600},GameViewSizeMode::FreeAspect,0,0,2,false,true,1,{});
    EXPECT_EQ(dpi.width,1600u);EXPECT_EQ(dpi.height,1200u);
    const auto oneToOne=GameViewPresentation::Resolve({},{800,600},GameViewSizeMode::FixedResolution,1920,1080,2,false,false,1,{});
    EXPECT_FLOAT_EQ(oneToOne.imageMax.x-oneToOne.imageMin.x,960);
}
TEST(GameViewInputRouterTests, OcclusionBlackBarsAndEscapeDoNotLeakInput)
{
    GameViewInputRouter router;
    const auto view=GameViewPresentation::Resolve({},{800,600},GameViewSizeMode::FixedResolution,1920,1080,1,false,true,1,{});
    GameInputFrame raw;raw.keys[87]=true;raw.mousePressed[0]=true;
    EXPECT_FALSE(router.Route(raw,view,{400,300},false,true,true,4).focused);
    EXPECT_FALSE(router.Route(raw,view,{400,10},true,true,true,4).focused);
    auto frame=router.Route(raw,view,{400,300},true,true,true,4);EXPECT_TRUE(frame.focused);EXPECT_TRUE(frame.keys[87]);
    raw.mousePressed[0]=false;raw.pressed[256]=true;
    frame=router.Route(raw,view,{400,300},true,true,true,4);EXPECT_FALSE(frame.focused);EXPECT_FALSE(frame.keys[87]);
}
TEST(PlayAudioTests, MuteAdvancesPauseHoldsAndStopReleasesSessionVoices)
{
    using namespace NLS::Engine::Audio;
    auto device=AudioDevice::CreateOffline();device->BeginSession(9);
    auto pcm=std::make_shared<AudioPcm>();pcm->channels=1;pcm->sampleRate=48000;pcm->samples.assign(48000,0.5f);
    const auto voice=device->Play(9,pcm,true,1);ASSERT_NE(voice,0u);
    std::vector<float> output(960);
    device->Render(output);EXPECT_NEAR(output[0],0.5f,1e-6);
    device->SetMuted(9,true);device->Render(output);EXPECT_EQ(output[0],0);
    EXPECT_NEAR(device->GetVoicePosition(voice),0.02,1e-6);
    device->SetPaused(9,true);device->Render(output);EXPECT_NEAR(device->GetVoicePosition(voice),0.02,1e-6);
    device->EndSession(9);EXPECT_EQ(device->GetVoiceCount(9),0u);EXPECT_FALSE(device->IsVoicePlaying(voice));
}

TEST(PlayAudioTests, DecodesRealWavMp3OggAndRunsWithoutADevice)
{
    using namespace NLS::Engine::Audio;
    for(const auto* extension:{"wav","mp3","ogg"})
    {
        const auto path=std::filesystem::path(NLS_ROOT_DIR)/"Tests"/"Fixtures"/"PlayAudio"/(std::string("tone.")+extension);
        std::ifstream file(path,std::ios::binary);const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(file),{});
        std::string error;const auto pcm=DecodeAudio(bytes,error);ASSERT_TRUE(pcm)<<extension<<": "<<error;
        EXPECT_EQ(pcm->channels,1u);EXPECT_EQ(pcm->sampleRate,48000u);EXPECT_GT(pcm->FrameCount(),4000u);
        EXPECT_TRUE(std::any_of(pcm->samples.begin(),pcm->samples.end(),[](float value){return std::abs(value)>.001f;}));
        auto device=AudioDevice::CreateOffline();EXPECT_FALSE(device->IsDeviceAvailable());device->BeginSession(1);
        const auto voice=device->Play(1,pcm,true,1);ASSERT_NE(voice,0u);
        device->TickSilent(.01f);EXPECT_NEAR(device->GetVoicePosition(voice),.01,1e-5);
        device->SetPaused(1,true);device->TickSilent(2);EXPECT_NEAR(device->GetVoicePosition(voice),.01,1e-5);
        device->EndSession(1);EXPECT_EQ(device->GetVoiceCount(1),0u);
        EXPECT_EQ(device->Play(1,pcm,true,1),0u);
    }
    std::string error;EXPECT_FALSE(DecodeAudio({},error));EXPECT_FALSE(error.empty());
}
TEST(PlayPhysicsTests, DynamicBodyFallsAndProducesContactInExistingQueryWorld)
{
    using namespace NLS::Physics;
    std::string error;auto world=PhysicsWorld::Create(error);ASSERT_TRUE(world)<<error;
    BodySettings floor;floor.position={0,-1,0};ASSERT_TRUE(world->AddBox({10,0.5f,10},floor).IsValid());
    BodySettings falling;falling.position={0,2,0};falling.dynamic=true;falling.kinematic=false;
    const auto body=world->AddSphere(0.5f,falling);ASSERT_TRUE(body.IsValid());
    bool entered=false;
    for(int frame=0;frame<100;++frame)
    {world->Step(0.02f);for(const auto& event:world->TakeContactEvents())entered|=event.phase==ContactPhase::Enter;}
    NLS::Maths::Vector3 position;NLS::Maths::Quaternion rotation;ASSERT_TRUE(world->GetTransform(body,position,rotation));
    EXPECT_GT(position.y,-0.1f);EXPECT_LT(position.y,0.15f);EXPECT_TRUE(entered);
}

TEST(PlayModeControllerTests, QueuesTransitionsAndAdvancesOnlyOneStep)
{
    PlayModeController controller;
    int entered = 0, stopped = 0;
    controller.SetCallbacks({[&](uint64_t id, std::string&) { EXPECT_GT(id, 0u); ++entered; return true; },
        [&](std::string&) { ++stopped; return true; }, [](bool) {}});
    controller.RequestPlay();
    EXPECT_EQ(controller.GetState(), PlaySessionState::Edit);
    controller.ProcessRequests();
    EXPECT_EQ(controller.GetState(), PlaySessionState::Playing);
    controller.RequestStep(); controller.RequestStep(); controller.ProcessRequests();
    EXPECT_EQ(controller.GetState(), PlaySessionState::Stepping);
    controller.CompleteStep();
    EXPECT_EQ(controller.GetState(), PlaySessionState::Paused);
    controller.RequestResume(); controller.ProcessRequests();
    EXPECT_EQ(controller.GetState(), PlaySessionState::Playing);
    controller.RequestTogglePlay(); controller.ProcessRequests();
    EXPECT_EQ(controller.GetState(), PlaySessionState::Edit);
    EXPECT_EQ(entered, 1); EXPECT_EQ(stopped, 1);
}
TEST(PlayModeControllerTests, RecoveryFailureCannotPublishEditAndCanRetry)
{
    PlayModeController controller;
    bool canRestore = false;
    controller.SetCallbacks({[](uint64_t, std::string&) { return true; },
        [&](std::string& error) { error = "injected restore failure"; return canRestore; }, {}});
    controller.RequestPlay(); controller.ProcessRequests();
    controller.RequestStop(); controller.ProcessRequests();
    EXPECT_EQ(controller.GetState(), PlaySessionState::Faulted);
    canRestore = true;
    controller.RequestStop(); controller.ProcessRequests();
    EXPECT_EQ(controller.GetState(), PlaySessionState::Edit);
}
TEST(PlayModeControllerTests, StopAfterStepWinsWithoutAnotherSimulation)
{
    PlayModeController controller;
    controller.SetCallbacks({[](uint64_t, std::string&) { return true; }, [](std::string&) { return true; }, {}});
    controller.RequestPlay(); controller.ProcessRequests();
    controller.RequestStep(); controller.RequestStop(); controller.ProcessRequests();
    EXPECT_EQ(controller.GetState(), PlaySessionState::Edit);
}
TEST(RuntimePlaySessionTests, FixedStepIsScaledOnceAndStepAlwaysRunsOneFixedTick)
{
    ScriptRuntime scripts;
    ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");
    RuntimePlaySession session;
    ASSERT_TRUE(session.Begin(scripts, scenes, 23).Succeeded());
    scenes.GetCurrentScene()->Play();
    ScriptEngineApi::SetFrameTiming(0.02f, 0.1f);
    ScriptEngineApi::SetTimeScale(2);
    session.Tick(0.05f);
    EXPECT_NEAR(session.GetFrame().time, 0.1, 1e-6);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex, 5u);
    session.End();
    scenes.LoadEmptyScene();
    ASSERT_TRUE(session.Begin(scripts, scenes, 24).Succeeded());
    scenes.GetCurrentScene()->Play();
    session.Step();
    EXPECT_EQ(session.GetFrame().frameIndex, 1u);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex, 1u);
    session.Step();
    EXPECT_EQ(session.GetFrame().fixedFrameIndex, 2u);
    session.End();
}

TEST(RuntimePlaySessionTests, ManualStepIgnoresTimeScaleAndPreservesOrdinaryFractionalDebt)
{
    ScriptRuntime scripts;
    ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");
    RuntimePlaySession session;
    ASSERT_TRUE(session.Begin(scripts, scenes, 25).Succeeded());
    scenes.GetCurrentScene()->Play();
    const auto interval = session.GetFrame().fixedDeltaTime;
    session.Tick(interval * 0.25f);
    const auto debt = session.GetPhysicsClock().GetAccumulatedTime();
    session.SetPaused(true);
    for (const float scale : {0.0f, 0.5f, 1.0f, 2.0f})
    {
        ScriptEngineApi::SetTimeScale(scale);
        const auto before = session.GetFrame();
        session.Step();
        EXPECT_EQ(session.GetFrame().fixedFrameIndex, before.fixedFrameIndex + 1);
        EXPECT_EQ(session.GetFrame().frameIndex, before.frameIndex + 1);
        EXPECT_NEAR(session.GetFrame().time - before.time, interval, 1e-6);
        EXPECT_DOUBLE_EQ(session.GetPhysicsClock().GetAccumulatedTime(), debt);
        session.Tick(10);
        EXPECT_EQ(session.GetFrame().frameIndex, before.frameIndex + 1);
    }
    ScriptEngineApi::SetTimeScale(1);
    session.SetPaused(false);
    session.Tick(interval * 0.75f);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex, 5u);
    EXPECT_TRUE(session.End().Succeeded());
}

TEST(RuntimePlaySessionTests, PublishesRenderInterpolationAndPauseStepShowsLatestPhysicalPose)
{
    using namespace NLS;
    ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    Engine::SceneSystem::SceneManager scenes(""); RuntimePlaySession session;
    ASSERT_TRUE(session.Begin(scripts,scenes,2301).Succeeded());
    auto* scene=scenes.GetCurrentScene(); auto settings=scene->GetPhysicsSettings(); settings.interpolate=true;
    std::string error; ASSERT_TRUE(scene->SetPhysicsSettings(settings,error));
    auto& object=scene->CreateGameObject("Presentation");
    auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); body->SetUseGravity(false); body->SetVelocity({10,0,0});
    object.AddComponent<Engine::Components::PhysicsColliderComponent>();
    scene->Play(); ScriptEngineApi::SetTimeScale(1);
    const auto h=session.GetFrame().fixedDeltaTime;
    session.Tick(h*1.5f); ASSERT_EQ(session.GetFrame().fixedFrameIndex,1u);
    EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x,10*h,1e-5f);
    EXPECT_NEAR(scene->CapturePhysicsPresentationTransform(object).worldPosition.x,5*h,1e-5f);
    const auto debt=session.GetPhysicsClock().GetAccumulatedTime();
    session.SetPaused(true);
    EXPECT_NEAR(scene->CapturePhysicsPresentationTransform(object).worldPosition.x,10*h,1e-5f);
    session.Tick(100); EXPECT_EQ(session.GetFrame().fixedFrameIndex,1u);
    ScriptEngineApi::SetTimeScale(0); session.Step();
    EXPECT_EQ(session.GetFrame().fixedFrameIndex,2u);
    EXPECT_NEAR(scene->CapturePhysicsPresentationTransform(object).worldPosition.x,20*h,1e-5f);
    EXPECT_DOUBLE_EQ(session.GetPhysicsClock().GetAccumulatedTime(),debt);
    session.SetPaused(false); session.Tick(h);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex,2u);
    EXPECT_NEAR(scene->CapturePhysicsPresentationTransform(object).worldPosition.x,20*h,1e-5f);
    ScriptEngineApi::SetTimeScale(1); session.Tick(h);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex,3u);
    EXPECT_NEAR(scene->CapturePhysicsPresentationTransform(object).worldPosition.x,25*h,1e-5f);
    EXPECT_TRUE(session.End().Succeeded());
}

TEST(RuntimePlaySessionTests, CollisionScriptDispatchDefersStructuralAndMotionCommitsUnderRetainedQueries)
{
    using namespace NLS;
    ScriptApiDatabase api; ScriptTypeDescriptor type; type.name="PhysicsMutationProbe"; type.id=MakeStableScriptId(type.name); ASSERT_TRUE(api.RegisterType(type));
    auto backend=std::make_unique<FakeScriptBackend>(); auto* observer=backend.get(); ScriptRuntime scripts;
    ASSERT_TRUE(scripts.RegisterBackend(std::move(backend)).Succeeded()); ASSERT_TRUE(scripts.Initialize(api).Succeeded());
    Engine::SceneSystem::SceneManager scenes(""); auto* scene=scenes.GetCurrentScene();
    auto& floor=scene->CreateGameObject("Floor"); floor.GetTransform()->SetWorldPosition({0,-0.5f,0});
    floor.AddComponent<Engine::Components::PhysicsColliderComponent>()->SetSize({5,0.5f,5});
    auto& object=scene->CreateGameObject("Collision owner"); object.GetTransform()->SetWorldPosition({0,0.4f,0});
    auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); body->SetUseGravity(false);
    auto* collider=object.AddComponent<Engine::Components::PhysicsColliderComponent>();
    auto* script=object.AddComponent<ScriptComponent>(); ScriptAsset asset;
    asset.assetId=Core::Assets::AssetId::New(); asset.language=ScriptLanguage::Lua; asset.scriptType=type.id; asset.sourcePath="PhysicsMutationProbe.lua";
    script->SetScriptAsset(asset); RuntimePlaySession session; ASSERT_TRUE(session.Begin(scripts,scenes,8765).Succeeded());
    scene->Play(); scene->SyncTransforms(); const auto world=scene->GetPhysicsWorld(); int delivered=0;
    observer->SetInvocationObserver([&](ScriptInstanceHandle,ScriptCallback callback,const ScriptInvocationContext&)
    {
        if (callback!=ScriptCallback::OnCollisionEnter) return;
        ++delivered; auto read=world->Read(); const auto revision=world->GetRevision();
        EXPECT_TRUE(scene->IsPhysicsMutationDeferred()); ASSERT_TRUE(object.RemoveComponent(collider));
        body->Teleport({100,3,0},Maths::Quaternion::Identity,false); body->ApplyPendingCommands();
        EXPECT_FALSE(scene->TrySyncTransforms()); EXPECT_EQ(world->GetRevision(),revision);
        Physics::QueryHit hit; EXPECT_TRUE(read->Raycast({0,5,0},{0,-1,0},10,hit));
    });
    ScriptEngineApi::SetTimeScale(0); session.Step(); ASSERT_EQ(delivered,1);
    Physics::BodyState state; ASSERT_TRUE(world->GetState(body->GetBodyHandle(),state)); EXPECT_LT(state.position.x,10);
    EXPECT_EQ(scene->GetPendingPhysicsCommandCount(),0u);
    session.Step(); ASSERT_TRUE(world->GetState(body->GetBodyHandle(),state)); EXPECT_NEAR(state.position.x,100,1e-5f);
    EXPECT_EQ(delivered,1); EXPECT_EQ(session.GetFrame().fixedFrameIndex,2u); EXPECT_TRUE(session.End().Succeeded());
}

TEST(RuntimePlaySessionTests, ConfiguredStepBudgetBoundsLongFramesAndCountsDiscardedTime)
{
    ScriptRuntime scripts;
    ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");
    RuntimePlaySession session;
    std::string error;
    ASSERT_TRUE(session.ConfigurePhysicsTiming({0.001, 8, 0.25}, error)) << error;
    ASSERT_TRUE(session.Begin(scripts, scenes, 26).Succeeded());
    scenes.GetCurrentScene()->Play();
    session.Tick(1);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex, 8u);
    EXPECT_GT(session.GetDroppedPhysicsTime(), 0.989);
    EXPECT_LT(session.GetPhysicsClock().GetAccumulatedTime(), session.GetFrame().fixedDeltaTime);
    const auto count = session.GetFrame().fixedFrameIndex;
    session.Tick(0);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex, count);
    EXPECT_TRUE(session.End().Succeeded());
}

TEST(RuntimePlaySessionTests, CollisionStopPreservesCallbackLifetimeAndCancelsRemainingCatchupWork)
{
    using namespace NLS;
    ScriptApiDatabase api; ScriptTypeDescriptor type; type.name="PhysicsStopProbe"; type.id=MakeStableScriptId(type.name); ASSERT_TRUE(api.RegisterType(type));
    auto backend=std::make_unique<FakeScriptBackend>(); auto* observer=backend.get(); ScriptRuntime scripts;
    ASSERT_TRUE(scripts.RegisterBackend(std::move(backend)).Succeeded()); ASSERT_TRUE(scripts.Initialize(api).Succeeded());
    Engine::SceneSystem::SceneManager scenes(""); auto* edit=scenes.GetCurrentScene(); std::string error; ASSERT_TRUE(scenes.BeginPlaySandbox(&error));
    auto* scene=scenes.GetCurrentScene();
    auto& floor=scene->CreateGameObject("Floor"); floor.GetTransform()->SetWorldPosition({0,-0.5f,0});
    floor.AddComponent<Engine::Components::PhysicsColliderComponent>()->SetSize({5,0.5f,5});
    auto& object=scene->CreateGameObject("Stop on contact"); object.GetTransform()->SetWorldPosition({0,0.4f,0});
    auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); body->SetUseGravity(false); object.AddComponent<Engine::Components::PhysicsColliderComponent>();
    auto& flat=scene->CreateGameObject("Planar"); flat.AddComponent<Engine::Components::Rigidbody2DComponent>(); flat.AddComponent<Engine::Components::PhysicsCollider2DComponent>();
    auto* script=object.AddComponent<ScriptComponent>(); ScriptAsset asset;
    asset.assetId=Core::Assets::AssetId::New(); asset.language=ScriptLanguage::Lua; asset.scriptType=type.id; asset.sourcePath="PhysicsStopProbe.lua"; script->SetScriptAsset(asset);
    RuntimePlaySession session; ASSERT_TRUE(session.ConfigurePhysicsTiming({0.01,8,0.25},error)); ASSERT_TRUE(session.Begin(scripts,scenes,8766).Succeeded());
    scene->Play(); scene->SyncTransforms(); const auto world=scene->GetPhysicsWorld(); const auto planar=scene->GetPhysicsWorld2D(); int entered=0, updates=0, late=0;
    observer->SetInvocationObserver([&](ScriptInstanceHandle,ScriptCallback callback,const ScriptInvocationContext&)
    {
        updates+=callback==ScriptCallback::Update; late+=callback==ScriptCallback::LateUpdate;
        if (callback!=ScriptCallback::OnCollisionEnter) return;
        ++entered; const auto read=world->Read(); const auto read2D=planar->Read(); const auto revision=world->GetRevision(), revision2D=planar->GetRevision();
        EXPECT_FALSE(session.End().Succeeded()); EXPECT_TRUE(session.IsActive());
        scenes.LoadEmptyScene(); EXPECT_FALSE(scenes.DiscardPlaySandbox(&error)); EXPECT_FALSE(error.empty());
        EXPECT_EQ(scenes.GetCurrentScene(),scene); scene->Stop(); EXPECT_TRUE(scene->IsPhysicsStopPending()); EXPECT_TRUE(scene->IsPlaying());
        body->AddImpulse({50,0,0}); EXPECT_FALSE(scene->TrySyncTransforms());
        EXPECT_EQ(world->GetRevision(),revision); EXPECT_EQ(planar->GetRevision(),revision2D);
    });
    session.Tick(0.08f); EXPECT_EQ(entered,1); EXPECT_EQ(updates,0); EXPECT_EQ(late,0);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex,1u); EXPECT_EQ(world->GetStatistics().stepIndex,1u); EXPECT_EQ(planar->GetStatistics().stepIndex,1u);
    EXPECT_FALSE(scene->IsPlaying()); EXPECT_FALSE(scene->IsPhysicsStopPending());
    EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(planar->GetStatistics().bodyCount,0u);
    EXPECT_TRUE(session.End().Succeeded()); ASSERT_TRUE(scenes.DiscardPlaySandbox(&error)); EXPECT_EQ(scenes.GetCurrentScene(),edit);
}

TEST(RuntimePlaySessionTests, ZeroTimeScaleStepCommitsWorldSettingsQueuedByRealCollisionScripts)
{
    using namespace NLS;
    ScriptApiDatabase api; ScriptTypeDescriptor type; type.name="PhysicsSettingsProbe"; type.id=MakeStableScriptId(type.name); ASSERT_TRUE(api.RegisterType(type));
    auto backend=std::make_unique<FakeScriptBackend>(); auto* observer=backend.get(); ScriptRuntime scripts;
    ASSERT_TRUE(scripts.RegisterBackend(std::move(backend)).Succeeded()); ASSERT_TRUE(scripts.Initialize(api).Succeeded());
    Engine::SceneSystem::SceneManager scenes(""); auto* scene=scenes.GetCurrentScene(); std::string error;
    auto settings=scene->GetPhysicsSettings(); settings.gravity3D={0,0,0}; settings.gravity2D={0,0}; ASSERT_TRUE(scene->SetPhysicsSettings(settings,error));
    auto& floor=scene->CreateGameObject("Floor"); floor.GetTransform()->SetWorldPosition({0,-0.5f,0});
    auto* floorCollider=floor.AddComponent<Engine::Components::PhysicsColliderComponent>(); floorCollider->SetSize({5,0.5f,5}); floorCollider->SetFriction(0);
    auto& object=scene->CreateGameObject("Configure on contact"); object.GetTransform()->SetWorldPosition({0,0.4f,0});
    auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); object.AddComponent<Engine::Components::PhysicsColliderComponent>()->SetFriction(0);
    auto& flat=scene->CreateGameObject("Planar"); auto* body2D=flat.AddComponent<Engine::Components::Rigidbody2DComponent>(); flat.AddComponent<Engine::Components::PhysicsCollider2DComponent>();
    auto* script=object.AddComponent<ScriptComponent>(); ScriptAsset asset;
    asset.assetId=Core::Assets::AssetId::New(); asset.language=ScriptLanguage::Lua; asset.scriptType=type.id; asset.sourcePath="PhysicsSettingsProbe.lua"; script->SetScriptAsset(asset);
    RuntimePlaySession session; ASSERT_TRUE(session.ConfigurePhysicsTiming({0.01,8,0.25},error)); ASSERT_TRUE(session.Begin(scripts,scenes,8767).Succeeded());
    scene->Play(); scene->SyncTransforms(); const auto world=scene->GetPhysicsWorld(); const auto planar=scene->GetPhysicsWorld2D(); int delivered=0;
    const auto original=body->GetBodyHandle(); const auto original2D=body2D->GetBodyHandle();
    observer->SetInvocationObserver([&](ScriptInstanceHandle,ScriptCallback callback,const ScriptInvocationContext&)
    {
        if (callback!=ScriptCallback::OnCollisionEnter || delivered) return;
        ++delivered; const auto read=world->Read(); const auto read2D=planar->Read(); const auto revision=world->GetRevision(), revision2D=planar->GetRevision();
        settings.gravity3D={2,0,0}; settings.gravity2D={-3,0}; settings.solverSubsteps2D=8;
        ASSERT_TRUE(scene->SetPhysicsSettings(settings,error)); EXPECT_FALSE(scene->TrySyncTransforms());
        EXPECT_EQ(world->GetRevision(),revision); EXPECT_EQ(planar->GetRevision(),revision2D); EXPECT_EQ(planar->GetSolverSubsteps(),4u);
    });
    session.SetPaused(true); ScriptEngineApi::SetTimeScale(0); session.Step(); EXPECT_EQ(delivered,1); EXPECT_EQ(planar->GetSolverSubsteps(),8u);
    EXPECT_NEAR(body->GetVelocity().x,0,1e-5f); EXPECT_NEAR(body2D->GetVelocity().x,0,1e-5f);
    session.Step(); EXPECT_NEAR(body->GetVelocity().x,0.02f,1e-4f); EXPECT_NEAR(body2D->GetVelocity().x,-0.03f,1e-5f);
    EXPECT_EQ(body->GetBodyHandle(),original); EXPECT_EQ(body2D->GetBodyHandle(),original2D); EXPECT_EQ(session.GetFrame().fixedFrameIndex,2u);
    EXPECT_TRUE(session.End().Succeeded());
}

TEST(RuntimePlaySessionTests, DefaultLongFrameRunsEightStepsAndReportsClippedTime)
{
    ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes(""); RuntimePlaySession session;
    ASSERT_TRUE(session.Begin(scripts, scenes, 27).Succeeded()); scenes.GetCurrentScene()->Play();
    EXPECT_NEAR(session.GetFrame().fixedDeltaTime, 1.0/60, 1e-8);
    EXPECT_FLOAT_EQ(ScriptEngineApi::GetMaximumDeltaTime(),0.25f);
    session.Tick(1);
    EXPECT_EQ(session.GetFrame().fixedFrameIndex,8u);
    EXPECT_GT(session.GetDroppedPhysicsTime(),0.85);
    EXPECT_LT(session.GetPhysicsClock().GetAccumulatedTime(),session.GetFrame().fixedDeltaTime);
    EXPECT_TRUE(session.End().Succeeded());
}

TEST(RuntimePlaySessionTests, PausedZeroTimeScaleStepAdvancesBothNativeWorldsExactlyOnce)
{
    ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes(""); RuntimePlaySession session;
    ASSERT_TRUE(session.Begin(scripts, scenes, 28).Succeeded());
    auto& scene=*scenes.GetCurrentScene();
    auto& spatial=scene.CreateGameObject("Spatial"); auto* body=spatial.AddComponent<NLS::Engine::Components::RigidbodyComponent>();
    spatial.AddComponent<NLS::Engine::Components::PhysicsColliderComponent>();
    auto& planar=scene.CreateGameObject("Planar"); auto* body2D=planar.AddComponent<NLS::Engine::Components::Rigidbody2DComponent>();
    planar.AddComponent<NLS::Engine::Components::PhysicsCollider2DComponent>();
    scene.Play(); scene.SyncTransforms();
    session.SetPaused(true); ScriptEngineApi::SetTimeScale(0);
    for (uint64_t i=1;i<=3;++i)
    {
        session.Step();
        EXPECT_EQ(scene.PeekPhysicsWorld()->GetStatistics().stepIndex,i);
        EXPECT_EQ(scene.PeekPhysicsWorld2D()->GetStatistics().stepIndex,i);
    }
    EXPECT_LT(spatial.GetTransform()->GetWorldPosition().y,0); EXPECT_LT(planar.GetTransform()->GetWorldPosition().y,0);
    EXPECT_TRUE(body->GetBodyHandle().IsValid()); EXPECT_TRUE(body2D->GetBodyHandle().IsValid());
    EXPECT_TRUE(session.End().Succeeded());
}

namespace
{
std::shared_ptr<const NLS::Input::CompiledInput> PlayInputDefinition()
{
    using namespace NLS::Input;
    InputDefinition definition;
    definition.id = NLS::Guid::NewDeterministic("play-input-test");
    ActionDefinition action; action.id = NLS::Guid::NewDeterministic("play.jump"); definition.actions.push_back(action);
    ContextDefinition context; context.id = NLS::Guid::NewDeterministic("play.gameplay"); definition.defaultContext = context.id;
    BindingDefinition binding; binding.id = NLS::Guid::NewDeterministic("play.jump.space"); binding.action = action.id;
    binding.parts = {{{DeviceKind::Keyboard, ControlKind::Button, 32}, {1}}}; context.bindings.push_back(binding); definition.contexts.push_back(context);
    std::vector<std::string> diagnostics;
    return CompiledInput::Compile(std::move(definition), diagnostics);
}
void PlayInputClick(NLS::Input::InputSystem& system, bool down)
{
    NLS::Input::InputEvent event; event.device = system.GetKeyboard(); event.control = 32;
    event.type = down ? NLS::Input::InputEventType::ButtonDown : NLS::Input::InputEventType::ButtonUp;
    EXPECT_TRUE(system.SubmitEvent(event));
}
}

TEST(RuntimePlaySessionInputTests, ZeroAndMultiplePhysicsStepsNeverLoseOrRepeatActionCommands)
{
    using namespace NLS::Input;
    double now = 0; InputSystem system([&] { return now; });
    ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes(""); RuntimePlaySession session;
    session.ConfigureInput(system); ASSERT_TRUE(session.Begin(scripts, scenes, 1001).Succeeded()); scenes.GetCurrentScene()->Play();
    auto user = session.GetInputUser(); auto definition = PlayInputDefinition(); ASSERT_TRUE(definition);
    user->SetDefinition(definition); user->PushContext(definition->GetDefinition().defaultContext);
    session.NotifyFocus(true); system.EndFrame();
    int dynamic = 0, fixed = 0;
    auto a = user->BindAction(NLS::Guid::NewDeterministic("play.jump"), ActionPhase::Performed, [&](const auto&) { ++dynamic; });
    auto b = user->BindAction(NLS::Guid::NewDeterministic("play.jump"), ActionPhase::Performed, [&](const auto&) { ++fixed; }, UpdateMode::Fixed);
    for (int i = 0; i < 3; ++i) { PlayInputClick(system, true); PlayInputClick(system, false); }
    now += .001; session.NotifyFocus(true); EXPECT_EQ(dynamic, 0);
    session.Tick(.001f); EXPECT_EQ(dynamic, 3); EXPECT_EQ(fixed, 0); EXPECT_GT(user->GetPendingFixedEventCount(), 0u);
    system.EndFrame(); now += .1; session.NotifyFocus(true); session.Tick(.1f);
    EXPECT_EQ(fixed, 3); EXPECT_EQ(dynamic, 3); EXPECT_EQ(user->GetPendingFixedEventCount(), 0u);
    system.EndFrame(); now += .1; session.NotifyFocus(true); session.Tick(.1f); EXPECT_EQ(fixed, 3);
    EXPECT_TRUE(session.End().Succeeded());
}

TEST(RuntimePlaySessionInputTests, ScriptApiReadsIndependentDynamicAndFixedEventCursors)
{
    using namespace NLS::Input;
    double now = 0; InputSystem system([&] { return now; });
    ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes(""); RuntimePlaySession session;
    session.ConfigureInput(system); ASSERT_TRUE(session.Begin(scripts, scenes, 1010).Succeeded()); scenes.GetCurrentScene()->Play();
    auto user = session.GetInputUser(); auto definition = PlayInputDefinition(); user->SetDefinition(definition); user->PushContext(definition->GetDefinition().defaultContext);
    session.NotifyFocus(true); system.EndFrame();
    for (int i=0;i<3;++i) { PlayInputClick(system,true); PlayInputClick(system,false); }
    now += .001; session.NotifyFocus(true); session.Tick(.001f);
    const auto id = NLS::Guid::NewDeterministic("play.jump").ToString(); ScriptInputValue value;
    uint64_t cursor=0; int count=0;
    while (InputScriptApi::Read(0,id.c_str(),2,cursor,value) && (value.flags & 32)) { EXPECT_GT(value.sequence,cursor); cursor=value.sequence; count+=value.phase==static_cast<uint32_t>(ActionPhase::Performed); }
    EXPECT_EQ(count,3); ASSERT_TRUE(InputScriptApi::Read(0,id.c_str(),3,0,value)); EXPECT_EQ(value.flags,0u);
    system.EndFrame(); now += .1; session.NotifyFocus(true); session.Tick(.1f);
    // All three edges belonged to the first catch-up tick; its later ticks have
    // no repeated commands. Fixed subscribers consume them at that tick boundary.
    ASSERT_TRUE(InputScriptApi::Read(0,id.c_str(),3,0,value)); EXPECT_EQ(value.flags,0u);
    session.End(); EXPECT_FALSE(InputScriptApi::Read(0,id.c_str(),0,0,value));
}
TEST(RuntimePlaySessionInputTests, LuaSubscriptionsKeepEveryClickAndReloadWithoutStaleClosures)
{
#if NLS_HAS_LUA_VM
    using namespace NLS::Input;
    double now = 0; InputSystem system([&] { return now; });
    ScriptApiDatabase api; ScriptTypeDescriptor type; type.name="InputProbe"; type.id=MakeStableScriptId(type.name);
    type.fields.push_back({1,"count",{0,ScriptValueKind::Int32,"int",4,4,true}}); ASSERT_TRUE(api.RegisterType(type));
    ScriptRuntime scripts; ASSERT_TRUE(scripts.RegisterBackend(std::make_unique<LuaScriptBackend>()).Succeeded()); ASSERT_TRUE(scripts.Initialize(api).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes(""); RuntimePlaySession session;
    session.ConfigureInput(system);
    ScriptAsset asset;asset.assetId=NLS::Core::Assets::AssetId::New();asset.language=ScriptLanguage::Lua;asset.scriptType=type.id;asset.sourcePath="InputProbe.lua";
    const auto source=[&](int amount) { return "return {count=0, OnEnable=function(self) Nullus.Input.BindAction(self,'"+NLS::Guid::NewDeterministic("play.jump").ToString()+"',3,function(e) self.count=self.count+"+std::to_string(amount)+" end) end}"; };
    asset.sourceText=source(1);auto& object=scenes.GetCurrentScene()->CreateGameObject("InputProbe");auto* probe=object.AddComponent<ScriptComponent>();probe->SetScriptAsset(asset);
    ASSERT_TRUE(session.Begin(scripts,scenes,1011).Succeeded());
    auto user=session.GetInputUser();auto definition=PlayInputDefinition();user->SetDefinition(definition);user->PushContext(definition->GetDefinition().defaultContext);
    scenes.GetCurrentScene()->Play();session.NotifyFocus(true);system.EndFrame();
    const auto count=[&] { ScriptValue value; EXPECT_TRUE(probe->GetRuntimeSerializedField(1,value)); return std::get<int32_t>(value); };
    for(int i=0;i<2;++i){PlayInputClick(system,true);PlayInputClick(system,false);}now+=.01;session.NotifyFocus(true);session.Tick(.01f);EXPECT_EQ(count(),2);
    asset.sourceText=source(10); ASSERT_TRUE(scripts.ReloadAtFrameBoundary(asset,api,session.GetFrame()).Succeeded());
    system.EndFrame();PlayInputClick(system,true);PlayInputClick(system,false);now+=.01;session.NotifyFocus(true);session.Tick(.01f);EXPECT_EQ(count(),12);
    probe->SetEnabled(false); system.EndFrame(); PlayInputClick(system,true); PlayInputClick(system,false); now+=.01; session.NotifyFocus(true);
    probe->SetEnabled(true); session.Tick(.01f); EXPECT_EQ(count(),12);
    ASSERT_TRUE(ScriptEngineApi::QueueDestroy(NativeObjectHandle::FromInstanceId(object.GetInstanceID()),0));
    system.EndFrame();now+=.01;session.NotifyFocus(true);session.Tick(.01f);
    system.EndFrame();PlayInputClick(system,true);PlayInputClick(system,false);now+=.01;session.NotifyFocus(true);session.Tick(.01f);session.End();
#else
    GTEST_SKIP()<<"Lua VM is disabled.";
#endif
}
TEST(RuntimePlaySessionInputTests, LuaPairingPreservesGenerationTaggedDeviceIds)
{
#if NLS_HAS_LUA_VM
    using namespace NLS::Input;
    InputSystem system; EXPECT_GT(system.GetKeyboard().value, uint64_t{UINT32_MAX});
    const auto pad = system.RegisterDevice({DeviceKind::Gamepad, "Virtual Pad", "Gamepad"});
    ScriptApiDatabase api; ScriptTypeDescriptor type; type.name = "PairProbe"; type.id = MakeStableScriptId(type.name); ASSERT_TRUE(api.RegisterType(type));
    ScriptRuntime scripts; ASSERT_TRUE(scripts.RegisterBackend(std::make_unique<LuaScriptBackend>()).Succeeded()); ASSERT_TRUE(scripts.Initialize(api).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes(""); auto& object = scenes.GetCurrentScene()->CreateGameObject("PairProbe");
    auto* probe = object.AddComponent<ScriptComponent>(); ScriptAsset asset; asset.assetId = NLS::Core::Assets::AssetId::New(); asset.language = ScriptLanguage::Lua; asset.scriptType = type.id; asset.sourcePath = "PairProbe.lua";
    asset.sourceText = R"lua(return {Awake=function(self)
        assert(#Nullus.Input.GetUsers()==1)
        local devices=Nullus.Input.GetDevices(); assert(#devices==3)
        for _,device in ipairs(devices) do
            assert(device.connected and device.mapped)
            if device.kind==Nullus.Input.DeviceKind.Keyboard then Nullus.Input.UnpairDevice(device.id) end
            if device.kind==Nullus.Input.DeviceKind.Gamepad then Nullus.Input.PairDevice(device.id) end
        end
    end})lua";
    probe->SetScriptAsset(asset); RuntimePlaySession session; session.ConfigureInput(system);
    ASSERT_TRUE(session.Begin(scripts, scenes, 1020).Succeeded()); scenes.GetCurrentScene()->Play();
    EXPECT_FALSE(session.GetInputUser()->IsDevicePaired(system.GetKeyboard()));
    EXPECT_TRUE(session.GetInputUser()->IsDevicePaired(pad));
    uint64_t connected = 0; std::string error;
    ASSERT_TRUE(system.RemoveDevice(pad));
    EXPECT_TRUE(InputScriptApi::Command(0, 17, "", pad.value, connected, error)); EXPECT_EQ(connected, 0u);
    EXPECT_FALSE(InputScriptApi::Command(0, 6, "", pad.value, connected, error));
    session.End();
#else
    GTEST_SKIP() << "Lua VM is disabled.";
#endif
}
TEST(RuntimePlaySessionInputTests, RebindingTimeoutReportsItsStatusAfterReleasingTheInputLease)
{
    using namespace NLS::Input;
    double now = 0; InputSystem system([&] { return now; });
    ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded()); NLS::Engine::SceneSystem::SceneManager scenes("");
    RuntimePlaySession session; session.ConfigureInput(system); ASSERT_TRUE(session.Begin(scripts, scenes, 1021).Succeeded());
    auto user = session.GetInputUser(); const auto definition = PlayInputDefinition();
    user->SetDefinition(definition); user->PushContext(definition->GetDefinition().defaultContext);
    scenes.GetCurrentScene()->Play(); session.NotifyFocus(true); system.EndFrame();
    const auto binding = definition->GetDefinition().contexts[0].bindings[0].id.ToString();
    uint64_t token = 0, result = 0; std::string error;
    ASSERT_TRUE(InputScriptApi::Command(0, 8, binding.c_str(), 0, token, error)); EXPECT_TRUE(user->IsSuspended());
    ASSERT_TRUE(InputScriptApi::Command(0, 19, "", token, result, error)); EXPECT_EQ(result, 0u);
    now = 11; session.NotifyFocus(true); session.Tick(.001f); EXPECT_FALSE(user->IsSuspended());
    ASSERT_TRUE(InputScriptApi::Command(0, 9, "", token, result, error)); EXPECT_EQ(result, static_cast<uint64_t>(RebindingStatus::TimedOut));
    EXPECT_TRUE(InputScriptApi::Command(0, 19, "", token, result, error)); EXPECT_EQ(result, 0u);
    EXPECT_TRUE(InputScriptApi::Command(0, 11, "", token, result, error));
    EXPECT_TRUE(InputScriptApi::Command(0, 11, "", token, result, error)); session.End();
}

TEST(RuntimePlaySessionInputTests, PauseAndStopIsolateCommandsWithoutResettingHostDevices)
{
    using namespace NLS::Input;
    double now = 0; InputSystem system([&] { return now; });
    ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes(""); RuntimePlaySession session;
    session.ConfigureInput(system); ASSERT_TRUE(session.Begin(scripts, scenes, 1002).Succeeded()); scenes.GetCurrentScene()->Play();
    auto user = session.GetInputUser(); auto definition = PlayInputDefinition(); user->SetDefinition(definition); user->PushContext(definition->GetDefinition().defaultContext);
    session.NotifyFocus(true); system.EndFrame();
    PlayInputClick(system, true); session.NotifyFocus(true); EXPECT_GT(user->GetPendingFixedEventCount(), 0u);
    session.SetPaused(true); EXPECT_EQ(user->GetPendingFixedEventCount(), 0u); EXPECT_TRUE(system.IsButtonDown(system.GetKeyboard(), 32));
    session.SetPaused(false); system.EndFrame(); session.NotifyFocus(true);
    EXPECT_FALSE(user->WasActionPressed(user->FindAction(NLS::Guid::NewDeterministic("play.jump"))));
    EXPECT_TRUE(session.End().Succeeded()); EXPECT_TRUE(system.IsButtonDown(system.GetKeyboard(), 32));
    EXPECT_TRUE(session.Begin(scripts, scenes, 1003).Succeeded()); EXPECT_NE(session.GetInputUser(), user);
    EXPECT_EQ(session.GetInputUser()->GetPendingFixedEventCount(), 0u); session.End();
}

TEST(RuntimePlaySessionInputTests, ManualStepConsumesBufferedInputAtZeroTimeScaleAndResumeDiscardsPendingInput)
{
    using namespace NLS::Input;
    double now = 0; InputSystem system([&] { return now; });
    ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes(""); RuntimePlaySession session;
    session.ConfigureInput(system); ASSERT_TRUE(session.Begin(scripts, scenes, 1004).Succeeded()); scenes.GetCurrentScene()->Play();
    auto user = session.GetInputUser(); auto definition = PlayInputDefinition(); user->SetDefinition(definition); user->PushContext(definition->GetDefinition().defaultContext);
    session.NotifyFocus(true); session.SetPaused(true); session.Step(); EXPECT_EQ(session.GetFrame().fixedFrameIndex, 1u);
    system.EndFrame(); now += .1; PlayInputClick(system, true); PlayInputClick(system, false); session.NotifyFocus(true);
    const auto pending = user->GetPendingFixedEventCount(); EXPECT_GT(pending, 0u);
    int performed = 0; auto subscription = user->BindAction(NLS::Guid::NewDeterministic("play.jump"), ActionPhase::Performed, [&](const auto&) { ++performed; }, UpdateMode::Fixed);
    ScriptEngineApi::SetTimeScale(0); session.Step(); EXPECT_EQ(user->GetPendingFixedEventCount(), 0u); EXPECT_EQ(performed, 1);
    ScriptEngineApi::SetTimeScale(1); session.Step(); EXPECT_EQ(performed, 1);
    system.EndFrame(); now += .1; PlayInputClick(system, true); PlayInputClick(system, false); session.NotifyFocus(true);
    EXPECT_GT(user->GetPendingFixedEventCount(), 0u); session.SetPaused(false); EXPECT_EQ(user->GetPendingFixedEventCount(), 0u);
    session.End();
}
TEST(RuntimePlaySessionTests, ScriptHostRestoreFailureRetainsSceneUntilRetry)
{
    ScriptRuntime scripts;
    auto backend=std::make_unique<FakeScriptBackend>();auto* fake=backend.get();
    ASSERT_TRUE(scripts.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");
    RuntimePlaySession session;
    ASSERT_TRUE(session.Begin(scripts,scenes,35).Succeeded());
    scenes.GetCurrentScene()->Play();
    fake->FailNextInitializeForTesting();
    const auto failed=session.End();
    EXPECT_FALSE(failed.Succeeded());
    EXPECT_TRUE(session.IsActive());
    EXPECT_FALSE(scenes.GetCurrentScene()->IsPlaying());
    EXPECT_FALSE(scripts.IsInitialized());
    ASSERT_TRUE(session.End().Succeeded());
    EXPECT_FALSE(session.IsActive());
    EXPECT_TRUE(scripts.IsInitialized());

    fake->FailNextInitializeForTesting();
    EXPECT_FALSE(session.Begin(scripts,scenes,36).Succeeded());
    EXPECT_FALSE(session.IsActive());
    EXPECT_FALSE(scripts.IsInitialized());
    EXPECT_TRUE(scripts.EndPlaySession().Succeeded());
    EXPECT_TRUE(scripts.IsInitialized());
}
TEST(RuntimePlaySessionTests, EditorCanRetirePlayBeforeRestoringAnUnavailableScriptHost)
{
    ScriptRuntime scripts;
    auto backend = std::make_unique<FakeScriptBackend>();
    auto* fake = backend.get();
    ASSERT_TRUE(scripts.RegisterBackend(std::move(backend)).Succeeded());
    ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");
    auto* editScene = scenes.GetCurrentScene();
    auto& authored = editScene->CreateGameObject("Authored");
    authored.GetTransform()->SetLocalPosition({1, 2, 3});
    std::string error;
    ASSERT_TRUE(scenes.BeginPlaySandbox(&error)) << error;
    NLS::Input::InputSystem input;
    RuntimePlaySession session;
    session.ConfigureInput(input);
    ASSERT_TRUE(session.Begin(scripts, scenes, 37).Succeeded());
    scenes.GetCurrentScene()->Play();
    scenes.GetCurrentScene()->GetGameObjects().front()->GetTransform()->SetLocalPosition({8, 9, 10});
    ASSERT_NE(session.GetInputDomain(), nullptr);
    fake->FailNextInitializeForTesting();
    ASSERT_TRUE(session.End(false).Succeeded());
    EXPECT_FALSE(session.IsActive());
    EXPECT_EQ(session.GetInputDomain(), nullptr);
    EXPECT_FALSE(NLS::Core::ServiceLocator::Contains<NLS::Input::InputDomain>());
    EXPECT_EQ(scripts.GetPlaySessionId(), 0u);
    EXPECT_FALSE(scripts.IsInitialized());
    // Restoring editor metadata can fail independently of stopping gameplay.
    EXPECT_FALSE(scripts.EndPlaySession().Succeeded());
    ASSERT_TRUE(scenes.DiscardPlaySandbox(&error)) << error;
    EXPECT_EQ(scenes.GetCurrentScene(), editScene);
    EXPECT_EQ(authored.GetTransform()->GetLocalPosition(), NLS::Maths::Vector3(1, 2, 3));
    authored.GetTransform()->SetLocalPosition({4, 5, 6});
    EXPECT_EQ(authored.GetTransform()->GetLocalPosition(), NLS::Maths::Vector3(4, 5, 6));
    EXPECT_TRUE(scripts.EndPlaySession().Succeeded());
}

TEST(RuntimePlaySessionTests, OneHundredSandboxSessionsRetainTheEditWorld)
{
    ScriptRuntime scripts;
    ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");
    auto* original = scenes.GetCurrentScene();
    auto& authored=original->CreateGameObject("Unsaved authored object");
    authored.GetTransform()->SetLocalPosition({1,2,3});
    scenes.MarkCurrentSceneDirty();
    for (uint64_t id = 1; id <= 100; ++id)
    {
        std::string error;
        ASSERT_TRUE(scenes.BeginPlaySandbox(&error)) << error;
        RuntimePlaySession session;
        ASSERT_TRUE(session.Begin(scripts, scenes, id).Succeeded());
        scenes.GetCurrentScene()->Play();
        scenes.GetCurrentScene()->CreateGameObject("Runtime only");
        session.Step();
        if(id%10==0) {scenes.LoadEmptyScene();scenes.GetCurrentScene()->Play();}
        session.End();
        ASSERT_TRUE(scenes.DiscardPlaySandbox(&error)) << error;
        EXPECT_EQ(scenes.GetCurrentScene(), original);
        EXPECT_EQ(scripts.GetPlaySessionId(), 0u);
        EXPECT_TRUE(scenes.HasUnsavedSceneChanges());
        EXPECT_EQ(original->GetGameObjects().size(),1u);
        EXPECT_EQ(authored.GetTransform()->GetLocalPosition(),NLS::Maths::Vector3(1,2,3));
    }
}

TEST(RuntimePlaySessionTests, ScalesZeroHalfNormalAndDoubleWithoutPauseCatchup)
{
    ScriptRuntime scripts;ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");
    uint64_t id=1000;
    for(float scale:{0.0f,0.5f,1.0f,2.0f})
    {
        scenes.LoadEmptyScene();RuntimePlaySession session;
        ASSERT_TRUE(session.Begin(scripts,scenes,++id).Succeeded());scenes.GetCurrentScene()->Play();
        ScriptEngineApi::SetTimeScale(scale);
        for(int i=0;i<60;++i)session.Tick(1.0f/60);
        EXPECT_NEAR(session.GetFrame().time,scale,1e-5);
        EXPECT_EQ(session.GetFrame().fixedFrameIndex,static_cast<uint64_t>(std::round(scale/session.GetFrame().fixedDeltaTime)));
        const auto before=session.GetFrame();session.SetPaused(true);session.Tick(3600);
        EXPECT_EQ(session.GetFrame().time,before.time);EXPECT_EQ(session.GetFrame().unscaledTime,before.unscaledTime);
        session.SetPaused(false);session.Tick(1.0f/60);
        EXPECT_NEAR(session.GetFrame().time-before.time,scale/60,1e-6);
        session.End();
    }
}
TEST(PlayScriptsTests, LuaSessionRecreatesVmAndCannotReusePreviousInstance)
{
#if NLS_HAS_LUA_VM
    ScriptApiDatabase api;ScriptTypeDescriptor type;type.name="PlayCounter";type.id=MakeStableScriptId(type.name);
    type.fields.push_back({42,"count",{0,ScriptValueKind::Int32,"int",4,4,true}});ASSERT_TRUE(api.RegisterType(type));
    ScriptRuntime scripts;ASSERT_TRUE(scripts.RegisterBackend(std::make_unique<LuaScriptBackend>()).Succeeded());
    ASSERT_TRUE(scripts.Initialize(api).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");ScriptInstanceHandle previous;
    for(uint64_t id=1;id<=2;++id)
    {
        scenes.LoadEmptyScene();auto& object=scenes.GetCurrentScene()->CreateGameObject("Counter");
        auto* component=object.AddComponent<ScriptComponent>();
        ScriptAsset asset;asset.assetId=NLS::Core::Assets::AssetId::New();asset.language=ScriptLanguage::Lua;
        asset.scriptType=type.id;asset.sourcePath="Assets/PlayCounter.lua";
        asset.sourceText="local total=0; return {count=0, Awake=function(self) total=total+1;self.count=total end, Update=function(self) total=total+1;self.count=total end}";
        component->SetScriptAsset(asset);RuntimePlaySession session;
        ASSERT_TRUE(session.Begin(scripts,scenes,id).Succeeded());scenes.GetCurrentScene()->Play();
        ScriptValue value;ASSERT_TRUE(component->GetRuntimeSerializedField(42,value));EXPECT_EQ(std::get<int32_t>(value),1);
        if(previous.IsValid())EXPECT_NE(component->GetInstance(),previous);
        session.Step();ASSERT_TRUE(component->GetRuntimeSerializedField(42,value));EXPECT_EQ(std::get<int32_t>(value),2);
        previous=component->GetInstance();session.End();
    }
#else
    GTEST_SKIP()<<"Lua VM is disabled.";
#endif
}
TEST(PlayBindingsTests, NativeAdapterChangesRigidbodiesAndCameraOutput)
{
    NLS::Engine::SceneSystem::Scene scene;auto& object=scene.CreateGameObject("Binding owner");
    auto* body=object.AddComponent<NLS::Engine::Components::RigidbodyComponent>();
    ScriptAbiValue argument;argument.floatingValue=3.5;ScriptAbiValue output;
    const auto id=NativeObjectHandle::FromInstanceId(body->GetInstanceID()).value;
    ASSERT_EQ(InvokePlayMember(id,MakeStableScriptId("Nullus.Play::Rigidbody.mass.set"),&argument,1,&output).code,ScriptStatusCode::Ok);
    EXPECT_FLOAT_EQ(body->GetMass(),3.5f);
    EXPECT_EQ(InvokePlayMember(id,MakeStableScriptId("Nullus.Play::Rigidbody.mass.set"),nullptr,0,&output).code,ScriptStatusCode::Exception);
    auto* camera=object.AddComponent<NLS::Engine::Components::CameraComponent>();argument.floatingValue=7;
    ASSERT_EQ(InvokePlayMember(NativeObjectHandle::FromInstanceId(camera->GetInstanceID()).value,MakeStableScriptId("Nullus.Play::Camera.targetDisplay.set"),&argument,1,&output).code,ScriptStatusCode::Ok);
    EXPECT_EQ(camera->GetTargetDisplay(),7);
}

TEST(PlayScriptsTests, ContentAddressedAssemblyBindsThePortableAbiWithoutAnUnversionedAlias)
{
#if NLS_ENABLE_CSHARP_SCRIPTING
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto source = std::filesystem::path(NLS_BUILD_DIR) / "play-tests-managed" / NLS_BUILD_CONFIGURATION;
    const auto cache = std::filesystem::path(NLS_BUILD_DIR) / "play-cache-tests" / NLS::Guid::New().ToString();
    std::filesystem::create_directories(cache);
    for (const auto& entry : std::filesystem::directory_iterator(source))
        if (entry.is_regular_file() && entry.path().filename().string().rfind("GameScripts", 0) != 0)
            std::filesystem::copy_file(entry.path(), cache / entry.path().filename());
    for (const auto* extension : {".dll", ".deps.json", ".runtimeconfig.json"})
        std::filesystem::copy_file(source / (std::string("GameScripts") + extension),
            cache / (std::string("GameScripts.0123456789abcdef") + extension));
    ASSERT_FALSE(std::filesystem::exists(cache / "GameScripts.dll"));
    std::ifstream file(root / "Library/ScriptApi/ScriptApi.json", std::ios::binary);
    ScriptApiDatabase api;
    ASSERT_TRUE(ScriptApiManifest::FromJson(std::string(std::istreambuf_iterator<char>(file), {}), api).Succeeded());
    auto backend = std::make_unique<CoreClrScriptBackend>();
    auto* clr = backend.get();
#if defined(_WIN32)
    clr->SetDotnetRoot(root / "Tools/Dotnet/windows/x64");
#elif defined(__APPLE__)
    clr->SetDotnetRoot(root / "Tools/Dotnet/macos/x64");
#else
    clr->SetDotnetRoot(root / "Tools/Dotnet/linux/x64");
#endif
    clr->SetHostArtifacts(cache / "GameScripts.0123456789abcdef.runtimeconfig.json", cache / "GameScripts.0123456789abcdef.dll");
    ScriptRuntime scripts;
    ASSERT_TRUE(scripts.RegisterBackend(std::move(backend)).Succeeded());
    auto status = scripts.Initialize(api);
    ASSERT_TRUE(status.Succeeded()) << status.message;
    EXPECT_FALSE(clr->GetBehaviourManifest().empty());
    // Reopening through another spelling of the same runtime root must reuse
    // the process host rather than reject hostfxr's already-initialized status.
#if defined(_WIN32)
    clr->SetDotnetRoot(root / "Tools/Dotnet/windows/x64/.");
#elif defined(__APPLE__)
    clr->SetDotnetRoot(root / "Tools/Dotnet/macos/x64/.");
#else
    clr->SetDotnetRoot(root / "Tools/Dotnet/linux/x64/.");
#endif
    for (uint64_t id : {1001u, 1002u})
    {
        status = scripts.BeginPlaySession(id);
        ASSERT_TRUE(status.Succeeded()) << status.message;
        EXPECT_FALSE(clr->GetBehaviourManifest().empty());
        status = scripts.EndPlaySession();
        ASSERT_TRUE(status.Succeeded()) << status.message;
    }
#else
    GTEST_SKIP() << "CoreCLR is disabled.";
#endif
}

TEST(PlayScriptsTests, CSharpResetsStaticsSubscriptionsAndCollectsOldDomains)
{
#if NLS_ENABLE_CSHARP_SCRIPTING
    const auto root=std::filesystem::path(NLS_ROOT_DIR);
    const auto managed=std::filesystem::path(NLS_BUILD_DIR)/"play-tests-managed"/NLS_BUILD_CONFIGURATION;
    const auto read=[](const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(file),{});};
    ScriptApiDatabase api;ASSERT_TRUE(ScriptApiManifest::FromJson(read(root/"Library"/"ScriptApi"/"ScriptApi.json"),api).Succeeded());
    auto backend=std::make_unique<CoreClrScriptBackend>();auto* clr=backend.get();
#if defined(_WIN32)
    clr->SetDotnetRoot(root/"Tools"/"Dotnet"/"windows"/"x64");
#elif defined(__APPLE__)
    clr->SetDotnetRoot(root/"Tools"/"Dotnet"/"macos"/"x64");
#else
    clr->SetDotnetRoot(root/"Tools"/"Dotnet"/"linux"/"x64");
#endif
    clr->SetHostArtifacts(managed/"GameScripts.runtimeconfig.json",managed/"GameScripts.dll");
    ScriptRuntime scripts;ASSERT_TRUE(scripts.RegisterBackend(std::move(backend)).Succeeded());
    const auto initialized = scripts.Initialize(api);
    ASSERT_TRUE(initialized.Succeeded()) << initialized.message;
    NLS::Engine::SceneSystem::SceneManager scenes("");
    const auto field=[](const char* name){return MakeStableScriptId("global::Nullus.EngineScripts.PlaySessionProbe::"+std::string(name)+"::field");};
    for(uint64_t id=200;id<300;++id)
    {
        scenes.LoadEmptyScene();auto& object=scenes.GetCurrentScene()->CreateGameObject("Managed probe");
        auto* body=object.AddComponent<NLS::Engine::Components::RigidbodyComponent>();
        object.AddComponent<NLS::Engine::Components::PhysicsColliderComponent>();
        object.GetTransform()->SetWorldPosition({0,3,0});
        auto& floor=scenes.GetCurrentScene()->CreateGameObject("Floor");floor.GetTransform()->SetWorldPosition({0,-.5f,0});
        floor.AddComponent<NLS::Engine::Components::PhysicsColliderComponent>()->SetSize({5,.5f,5});
        auto& trigger=scenes.GetCurrentScene()->CreateGameObject("Trigger");trigger.GetTransform()->SetWorldPosition({0,1.4f,0});
        auto* triggerCollider=trigger.AddComponent<NLS::Engine::Components::PhysicsColliderComponent>();triggerCollider->SetTrigger(true);triggerCollider->SetSize({2,.1f,2});
        auto* component=object.AddComponent<ScriptComponent>();
        ScriptAsset asset;asset.assetId=NLS::Core::Assets::AssetId::New();asset.language=ScriptLanguage::CSharp;
        asset.sourcePath="Assets/PlaySessionProbe.cs";asset.sourceText=read(root/"Tests"/"Fixtures"/"PlayScripts"/"Assets"/"PlaySessionProbe.cs");
        component->SetScriptAsset(asset);RuntimePlaySession session;
        const auto begun=session.Begin(scripts,scenes,id);ASSERT_TRUE(begun.Succeeded())<<begun.message;
        scenes.GetCurrentScene()->Play();session.Step();
        ScriptValue value;ASSERT_TRUE(component->GetRuntimeSerializedField(field("AwakeSerial"),value));EXPECT_EQ(std::get<int32_t>(value),1);
        ASSERT_TRUE(component->GetRuntimeSerializedField(field("SignalsReceived"),value));EXPECT_EQ(std::get<int32_t>(value),1);
        ASSERT_TRUE(component->GetRuntimeSerializedField(field("WorkerApiRejected"),value));EXPECT_EQ(std::get<int32_t>(value),1);
        EXPECT_FLOAT_EQ(body->GetMass(),2);
        for(int frame=0;frame<180;++frame)session.Tick(1.f/60);
        for(const auto* name:{"CollisionEnterCount","CollisionStayCount","TriggerEnterCount","TriggerExitCount"})
        {ASSERT_TRUE(component->GetRuntimeSerializedField(field(name),value))<<name;EXPECT_GT(std::get<int32_t>(value),0)<<name;}
        session.SetPaused(true);
        ASSERT_TRUE(component->GetRuntimeSerializedField(field("PauseCount"),value));EXPECT_EQ(std::get<int32_t>(value),1);
        session.End();
        void* collect=nullptr;
        ASSERT_TRUE(clr->GetHost().GetUnmanagedFunctionPointer(managed/"Nullus.Managed.dll","Nullus.ManagedExports, Nullus.Managed", "CollectAndGetLiveProjectLoadContextCount",collect).Succeeded());
        ASSERT_NE(collect,nullptr);
        EXPECT_LE(reinterpret_cast<int(*)()>(collect)(),1);
    }
#else
    GTEST_SKIP()<<"CoreCLR is disabled.";
#endif
}

TEST(RuntimePlaySessionInputTests, CSharpSubscriptionsConsumeEachDynamicAndFixedClickOnce)
{
#if NLS_ENABLE_CSHARP_SCRIPTING
    const auto root=std::filesystem::path(NLS_ROOT_DIR), managed=std::filesystem::path(NLS_BUILD_DIR)/"play-tests-managed"/NLS_BUILD_CONFIGURATION;
    const auto read=[](const auto& path){std::ifstream file(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(file),{});};
    ScriptApiDatabase api;ASSERT_TRUE(ScriptApiManifest::FromJson(read(root/"Library/ScriptApi/ScriptApi.json"),api).Succeeded());
    auto backend=std::make_unique<CoreClrScriptBackend>();
#if defined(_WIN32)
    backend->SetDotnetRoot(root/"Tools/Dotnet/windows/x64");
#elif defined(__APPLE__)
    backend->SetDotnetRoot(root/"Tools/Dotnet/macos/x64");
#else
    backend->SetDotnetRoot(root/"Tools/Dotnet/linux/x64");
#endif
    backend->SetHostArtifacts(managed/"GameScripts.runtimeconfig.json",managed/"GameScripts.dll");
    ScriptRuntime scripts;ASSERT_TRUE(scripts.RegisterBackend(std::move(backend)).Succeeded());
    const auto initialized = scripts.Initialize(api);
    ASSERT_TRUE(initialized.Succeeded()) << initialized.message;
    double now=0;NLS::Input::InputSystem system([&]{return now;});NLS::Engine::SceneSystem::SceneManager scenes("");
    for (uint64_t id : {1012u,1013u})
    {
        scenes.LoadEmptyScene();auto& object=scenes.GetCurrentScene()->CreateGameObject("InputProbe");auto* probe=object.AddComponent<ScriptComponent>();
        ScriptAsset asset;asset.assetId=NLS::Core::Assets::AssetId::New();asset.language=ScriptLanguage::CSharp;asset.sourcePath="Assets/InputActionProbe.cs";asset.sourceText=read(root/"Tests/Fixtures/PlayScripts/Assets/InputActionProbe.cs");probe->SetScriptAsset(asset);
        RuntimePlaySession session;session.ConfigureInput(system);ASSERT_TRUE(session.Begin(scripts,scenes,id).Succeeded());
        auto user=session.GetInputUser();auto definition=PlayInputDefinition();user->SetDefinition(definition);user->PushContext(definition->GetDefinition().defaultContext);
        scenes.GetCurrentScene()->Play();session.NotifyFocus(true);system.EndFrame();
        const auto count=[&](const char* name){ScriptValue value;EXPECT_TRUE(probe->GetRuntimeSerializedField(MakeStableScriptId("global::Nullus.EngineScripts.InputActionProbe::"+std::string(name)+"::field"),value));return std::get<int32_t>(value);};
        EXPECT_EQ(count("DeviceCount"),2); EXPECT_EQ(count("KeyboardConnected"),1);
        for(int i=0;i<3;++i){PlayInputClick(system,true);PlayInputClick(system,false);}now+=.001;session.NotifyFocus(true);session.Tick(.001f);
        EXPECT_EQ(count("DynamicCount"),3);EXPECT_EQ(count("FixedCount"),0);
        system.EndFrame();now+=.021;session.NotifyFocus(true);session.Tick(.021f);EXPECT_EQ(count("FixedCount"),3);
        system.EndFrame();now+=.1;session.NotifyFocus(true);session.Tick(.1f);EXPECT_EQ(count("DynamicCount"),3);EXPECT_EQ(count("FixedCount"),3);
        probe->SetEnabled(false);system.EndFrame();PlayInputClick(system,true);PlayInputClick(system,false);now+=.03;session.NotifyFocus(true);session.Tick(.03f);
        EXPECT_EQ(count("DynamicCount"),3);probe->SetEnabled(true);
        system.EndFrame();PlayInputClick(system,true);PlayInputClick(system,false);now+=.03;session.NotifyFocus(true);session.Tick(.03f);
        EXPECT_EQ(count("DynamicCount"),4);EXPECT_EQ(count("FixedCount"),4);
        session.End();system.EndFrame();
    }
#else
    GTEST_SKIP()<<"CoreCLR is disabled.";
#endif
}
TEST(PlayModeControllerTests, EntryExceptionRollsBackAndCrossThreadStopCancelsEntry)
{
    PlayModeController controller;int stops=0;bool throwOnEntry=true;
    controller.SetCallbacks({[&](uint64_t,std::string&){if(throwOnEntry)throw std::runtime_error("injected clone failure");
        std::thread producer([&]{controller.RequestStop();});producer.join();return true;},
        [&](std::string&){++stops;return true;},{}});
    controller.RequestPlay();controller.ProcessRequests();EXPECT_EQ(controller.GetState(),PlaySessionState::Edit);
    EXPECT_EQ(controller.GetError(),"injected clone failure");EXPECT_EQ(stops,1);
    throwOnEntry=false;controller.RequestPlay();controller.ProcessRequests();
    EXPECT_EQ(controller.GetState(),PlaySessionState::Edit);EXPECT_EQ(stops,2);
}

TEST(GameOutputTests, DisplayDepthAndViewportUseStableSceneOrder)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto* a=scene.CreateGameObject("a").AddComponent<NLS::Engine::Components::CameraComponent>();
    auto* b=scene.CreateGameObject("b").AddComponent<NLS::Engine::Components::CameraComponent>();
    auto* c=scene.CreateGameObject("c").AddComponent<NLS::Engine::Components::CameraComponent>();
    c->SetDepth(-1);b->SetTargetDisplay(1);
    const auto output=NLS::Engine::Rendering::GameOutput::Build(scene,0,3840,2160);
    ASSERT_EQ(output.cameras.size(),2u);EXPECT_EQ(output.cameras[0],c);EXPECT_EQ(output.cameras[1],a);
    EXPECT_EQ(NLS::Engine::Rendering::GameOutput::Build(scene,1,3840,2160).cameras,std::vector{b});
    c->SetDepth(0);const auto tied=NLS::Engine::Rendering::GameOutput::Build(scene,0,10,10);
    EXPECT_EQ(tied.cameras[0],a);EXPECT_EQ(tied.cameras[1],c);
}

TEST(PlayScriptsTests, LuaLifecycleActivationMutationAndExceptionsRemainIsolated)
{
#if NLS_HAS_LUA_VM
    ScriptApiDatabase api;ScriptTypeDescriptor type;type.name="LifecycleProbe";type.id=MakeStableScriptId(type.name);
    const char* fields[]={"awakes","enables","starts","disables","updates"};
    for(uint64_t i=0;i<5;++i)type.fields.push_back({i+1,fields[i],{0,ScriptValueKind::Int32,"int",4,4,true}});
    ASSERT_TRUE(api.RegisterType(type));ScriptRuntime scripts;
    ASSERT_TRUE(scripts.RegisterBackend(std::make_unique<LuaScriptBackend>()).Succeeded());ASSERT_TRUE(scripts.Initialize(api).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");scenes.LoadEmptyScene();
    ScriptAsset asset;asset.assetId=NLS::Core::Assets::AssetId::New();asset.language=ScriptLanguage::Lua;asset.scriptType=type.id;asset.sourcePath="LifecycleProbe.lua";
    asset.sourceText=R"lua(return {awakes=0,enables=0,starts=0,disables=0,updates=0,
        Awake=function(self)self.awakes=self.awakes+1 end,
        OnEnable=function(self)self.enables=self.enables+1 end,
        Start=function(self)self.starts=self.starts+1 end,
        OnDisable=function(self)self.disables=self.disables+1 end,
        Update=function(self)self.updates=self.updates+1;if self.gameObject.name=='Thrower' then error('expected isolated callback failure')end end})lua";
    auto& dormant=scenes.GetCurrentScene()->CreateGameObject("Dormant");dormant.SetActive(false);
    auto* probe=dormant.AddComponent<ScriptComponent>();probe->SetScriptAsset(asset);
    auto& thrower=scenes.GetCurrentScene()->CreateGameObject("Thrower");thrower.AddComponent<ScriptComponent>()->SetScriptAsset(asset);
    RuntimePlaySession session;ASSERT_TRUE(session.Begin(scripts,scenes,600).Succeeded());scenes.GetCurrentScene()->Play();
    EXPECT_FALSE(probe->GetInstance().IsValid());dormant.SetActive(true);
    const auto count=[&](uint64_t field){ScriptValue value;EXPECT_TRUE(probe->GetRuntimeSerializedField(field,value));return std::get<int32_t>(value);};
    EXPECT_EQ(count(1),1);EXPECT_EQ(count(2),1);EXPECT_EQ(count(3),0);
    probe->SetEnabled(false);session.Step();EXPECT_EQ(count(5),0);EXPECT_EQ(count(4),1);
    probe->SetEnabled(true);session.Step();EXPECT_EQ(count(1),1);EXPECT_EQ(count(2),2);EXPECT_EQ(count(3),1);EXPECT_EQ(count(5),1);
    session.Step();EXPECT_EQ(count(3),1);EXPECT_EQ(count(5),2);
    const auto destroyed=NativeObjectHandle::FromInstanceId(thrower.GetInstanceID());
    ASSERT_TRUE(ScriptEngineApi::QueueDestroy(destroyed,0));session.Step();
    EXPECT_EQ(NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(destroyed.value)),nullptr);
    EXPECT_EQ(count(5),3);session.End();
#else
    GTEST_SKIP()<<"Lua VM is disabled.";
#endif
}

TEST(PlayUiTests, DisplayFilteringAndPointerCancellationPreserveTextFocus)
{
#if defined(NLS_ENABLE_GAME_UI)
    NLS::Engine::Components::UICanvasComponent first,second;
    const auto source=R"json({"version":1,"root":{"type":"Panel","name":"Root","layout":{"size":[100,100]},"style":{"backgroundColor":[1,0,0,1]},"children":[{"type":"TextField","name":"Text","layout":{"size":[80,30]}}]}})json";
    ASSERT_TRUE(first.LoadDocumentJson(source));ASSERT_TRUE(second.LoadDocumentJson(source));second.SetTargetDisplay(1);
    NLS::Engine::UI::UIRuntimeService ui;ui.SyncCanvases({&first,&second});ui.Tick(0,{200,100},1,1);
    const auto display0=ui.BuildScreenSnapshot(200,100,1,1,0),display1=ui.BuildScreenSnapshot(200,100,1,1,1);
    EXPECT_EQ(display0->vertices.size(),display1->vertices.size());EXPECT_GT(display0->vertices.size(),0u);
    EXPECT_EQ(ui.BuildScreenSnapshot(200,100,1,1,7)->vertices.size(),0u);
    first.GetInputRouter().DispatchPointer(NLS::Engine::UI::UIEventType::PointerDown,{4,4});
    const auto* focused=first.GetInputRouter().GetFocusedElement();ASSERT_NE(focused,nullptr);
    ui.CancelInput(false);EXPECT_EQ(first.GetInputRouter().GetFocusedElement(),focused);
    ui.CancelInput();EXPECT_EQ(first.GetInputRouter().GetFocusedElement(),nullptr);
#else
    GTEST_SKIP()<<"Game UI is disabled.";
#endif
}

TEST(PlayUiTests, OrderedMouseClicksReachUiThreeTimesAndDoNotTriggerGameplay)
{
#if defined(NLS_ENABLE_GAME_UI)
    using namespace NLS::Input; namespace UI = NLS::Engine::UI;
    ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded());
    NLS::Engine::SceneSystem::SceneManager scenes("");
    auto* canvas = scenes.GetCurrentScene()->CreateGameObject("UI").AddComponent<NLS::Engine::Components::UICanvasComponent>();
    ASSERT_TRUE(canvas->LoadDocumentJson(R"json({"version":1,"root":{"type":"Panel","name":"Root","layout":{"size":[100,100]},"children":[{"type":"Button","name":"Button","layout":{"size":[80,30]}}]}})json"));
    auto* button = canvas->GetInstance()->GetElementByName("Button"); ASSERT_NE(button, nullptr);
    int clicks = 0; button->AddEventListener(UI::UIEventType::Click, [&](UI::UIEvent&) { ++clicks; });
    InputSystem system; RuntimePlaySession session; session.ConfigureInput(system); session.ConfigureUI("", 100, 100);
    ASSERT_TRUE(session.Begin(scripts, scenes, 909).Succeeded()); scenes.GetCurrentScene()->Play();
    auto data = PlayInputDefinition()->GetDefinition(); data.contexts[0].bindings[0].parts[0].path = {DeviceKind::Mouse, ControlKind::Button, 0};
    std::vector<std::string> diagnostics; auto user = session.GetInputUser(); user->SetDefinition(CompiledInput::Compile(data, diagnostics)); user->PushContext(data.contexts[0].id);
    int commands = 0; auto subscription = user->BindAction(data.actions[0].id, ActionPhase::Performed, [&](const auto&) { ++commands; });
    InputEvent event; event.device = system.GetMouse(); event.type = InputEventType::PointerMove; event.x = event.y = 4; ASSERT_TRUE(system.SubmitEvent(event));
    for (int i = 0; i < 3; ++i) { event.type = InputEventType::ButtonDown; ASSERT_TRUE(system.SubmitEvent(event)); event.type = InputEventType::ButtonUp; ASSERT_TRUE(system.SubmitEvent(event)); }
    GameInputFrame frame; frame.focused = true; frame.position = {4, 4}; frame.width = frame.height = 100;
    frame.sessionId = session.GetSessionId();
    frame.hasOrderedEvents = true; frame.sourceInputFrame = system.GetFrameIndex(); frame.orderedEvents = system.GetEvents();
    ScriptEngineApi::SetGameInputFrame(frame); session.NotifyFocus(true); session.Tick(.001f);
    EXPECT_EQ(clicks, 3); EXPECT_EQ(commands, 0); EXPECT_TRUE(user->BeginFixedTick(1, .02).events.empty());
    session.End();
#else
    GTEST_SKIP() << "Game UI is disabled.";
#endif
}
