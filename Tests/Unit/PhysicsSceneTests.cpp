#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <limits>
#include <type_traits>
#include <string>
#include <vector>

#include "Components/ParticleSystemComponent.h"
#include "Components/PhysicsColliderComponent.h"
#include "Components/RigidbodyComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphWriter.h"
#include <Physics/PhysicsWorld.h>
#include <Rendering/Entities/Camera.h>
#include "Rendering/SceneProxySourceRegistry.h"
#include "Rendering/RenderScene.h"
#include "../RenderingThreadTestHelpers.h"
#include <VFXCompiler/VfxCompiler.h>

namespace
{
using namespace NLS;
using ParticleCollider = Engine::Components::PhysicsColliderComponent;

std::shared_ptr<VFX::CompiledSystem> MakeCollidingEmitter()
{
    auto asset = VFX::CreateDefaultSystem();
    auto& emitter = asset.emitters[0];
    emitter.spawn.nodes.clear();
    emitter.spawnRate = 0;
    emitter.bursts = {{0, 1}};
    emitter.collisionEnabled = true;
    emitter.collisionRadius = 0.05f;
    emitter.attributes[0].defaultValue = {0, 1, 0, 0};
    emitter.attributes[1].defaultValue = {0, -10, 0, 0};
    auto compiled = std::make_shared<VFX::CompiledSystem>();
    std::vector<VFX::Diagnostic> diagnostics;
    EXPECT_TRUE(VFX::CompileSystem(asset, *compiled, diagnostics));
    return compiled;
}
}

TEST(PhysicsSceneTests, ColliderPublishesQueryBodiesAndTracksTransforms)
{
    Engine::SceneSystem::Scene scene;
    if (scene.GetPhysicsWorld() == nullptr)
        GTEST_SKIP() << "PhysX query world is unavailable";

    auto& object = scene.CreateGameObject("Box");
    auto* collider = object.AddComponent<ParticleCollider>();
    ASSERT_NE(collider, nullptr);
    collider->SetSize({0.5f, 0.5f, 0.5f});
    scene.Update(1.0f / 60);
    EXPECT_TRUE(collider->IsQueryReady());

    const auto& world = scene.GetPhysicsWorld();
    Physics::QueryHit hit;
    ASSERT_TRUE(world->Read()->Raycast({0, 3, 0}, {0, -1, 0}, 10, hit));
    EXPECT_NEAR(hit.position.y, 0.5f, 0.01f);

    const auto revision = scene.GetPhysicsRevision();
    object.GetTransform()->SetWorldPosition({2, 0, 0});
    scene.Update(1.0f / 60);
    EXPECT_GT(scene.GetPhysicsRevision(), revision);
    EXPECT_FALSE(world->Read()->Raycast({0, 3, 0}, {0, -1, 0}, 10, hit));
    ASSERT_TRUE(world->Read()->Raycast({2, 3, 0}, {0, -1, 0}, 10, hit));
    EXPECT_NEAR(hit.position.y, 0.5f, 0.01f);
}

TEST(PhysicsStructureTests, RuntimeCreationWaitsForSafeSynchronizationInBothDimensions)
{
    Engine::SceneSystem::Scene scene; scene.Play();
    const auto world=scene.GetPhysicsWorld(); ASSERT_TRUE(world);
    const auto world2D=scene.GetPhysicsWorld2D(); ASSERT_TRUE(world2D);
    auto& spatial=scene.CreateGameObject("Deferred spatial");
    auto& planar=scene.CreateGameObject("Deferred planar");
    auto* body=spatial.AddComponent<Engine::Components::RigidbodyComponent>();
    auto* collider=spatial.AddComponent<ParticleCollider>();
    auto* body2D=planar.AddComponent<Engine::Components::Rigidbody2DComponent>();
    auto* collider2D=planar.AddComponent<Engine::Components::PhysicsCollider2DComponent>();
    body->Synchronize(); collider->Synchronize(); body2D->Synchronize(); collider2D->Synchronize();
    EXPECT_FALSE(body->GetBodyHandle().IsValid()); EXPECT_FALSE(body2D->GetBodyHandle().IsValid());
    EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(world2D->GetStatistics().bodyCount,0u);
    ASSERT_TRUE(scene.TrySyncTransforms());
    EXPECT_TRUE(body->GetBodyHandle().IsValid()); EXPECT_TRUE(body2D->GetBodyHandle().IsValid());
    EXPECT_EQ(world->GetStatistics().shapeCount,1u); EXPECT_EQ(world2D->GetStatistics().shapeCount,1u);
    EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u);
}

TEST(PhysicsStructureTests, AuthoringDeferralKeepsQueriesStableUntilExplicitSynchronization)
{
    Engine::SceneSystem::Scene scene; auto& object=scene.CreateGameObject("Deferred authoring");
    auto* old=object.AddComponent<ParticleCollider>(); scene.SyncTransforms(); const auto world=scene.GetPhysicsWorld();
    const auto oldOwner=static_cast<uint64_t>(old->GetInstanceID()); ParticleCollider* replacement=nullptr;
    {
        const Engine::SceneSystem::Scene::PhysicsMutationDeferral deferral(scene);
        auto read=world->Read(); const auto revision=world->GetRevision();
        ASSERT_TRUE(object.RemoveComponent(old)); replacement=object.AddComponent<ParticleCollider>();
        EXPECT_FALSE(replacement->GetBodyHandle().IsValid()); EXPECT_FALSE(scene.TrySyncTransforms());
        EXPECT_EQ(world->GetRevision(),revision);
        Physics::QueryHit hit; ASSERT_TRUE(read->Raycast({0,3,0},{0,-1,0},10,hit)); EXPECT_EQ(hit.owner,oldOwner);
    }
    ASSERT_TRUE(scene.TrySyncTransforms()); EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u);
    Physics::QueryHit hit; ASSERT_TRUE(world->Read()->Raycast({0,3,0},{0,-1,0},10,hit));
    EXPECT_EQ(hit.owner,static_cast<uint64_t>(replacement->GetInstanceID())); EXPECT_NE(hit.owner,oldOwner);
    EXPECT_EQ(world->GetStatistics().bodyCount,1u); EXPECT_EQ(world->GetStatistics().shapeCount,1u);
}

TEST(PhysicsStructureTests, ComponentDestructionWithRetainedQueriesWaitsForTheNextSafeCommit)
{
    for (bool twoDimensional:{false,true})
    {
        Engine::SceneSystem::Scene scene; auto& object=scene.CreateGameObject("Retained query");
        Engine::Components::PhysicsCollider2DComponent* collider2D=nullptr; ParticleCollider* collider=nullptr;
        if (twoDimensional) { object.AddComponent<Engine::Components::Rigidbody2DComponent>(); collider2D=object.AddComponent<Engine::Components::PhysicsCollider2DComponent>(); }
        else { object.AddComponent<Engine::Components::RigidbodyComponent>(); collider=object.AddComponent<ParticleCollider>(); }
        scene.Play(); scene.SyncTransforms();
        const auto exercise=[&](auto world,auto* component)
        {
            const auto owner=static_cast<uint64_t>(component->GetInstanceID());
            {
                const Engine::SceneSystem::Scene::PhysicsMutationDeferral deferral(scene);
                auto read=world->Read();
                ASSERT_TRUE(object.RemoveComponent(component));
                EXPECT_GT(scene.GetPendingPhysicsCommandCount(),0u); EXPECT_FALSE(scene.TrySyncTransforms());
                if constexpr (std::is_same_v<std::remove_reference_t<decltype(*world)>,Physics::PhysicsWorld>)
                { Physics::QueryHit hit; ASSERT_TRUE(read->Raycast({0,3,0},{0,-1,0},10,hit)); EXPECT_EQ(hit.owner,owner); }
                else
                { Physics::QueryHit2D hit; ASSERT_TRUE(read->Raycast({0,3},{0,-1},10,hit)); EXPECT_EQ(hit.owner,owner); }
            }
            EXPECT_EQ(world->GetStatistics().shapeCount,1u);
            ASSERT_TRUE(scene.TrySyncTransforms());
            EXPECT_EQ(world->GetStatistics().shapeCount,0u); EXPECT_EQ(world->GetStatistics().bodyCount,1u);
            EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u);
        };
        if (twoDimensional) exercise(scene.GetPhysicsWorld2D(),collider2D);
        else exercise(scene.GetPhysicsWorld(),collider);
    }
}

TEST(PhysicsStructureTests, ContactCallbacksCanDestroyAndCreateObjectsWithoutMutatingTheirReadWorld)
{
    Engine::SceneSystem::Scene scene;
    auto& floor=scene.CreateGameObject("Floor"); floor.GetTransform()->SetWorldPosition({0,-0.5f,0});
    floor.AddComponent<ParticleCollider>()->SetSize({5,0.5f,5});
    auto& object=scene.CreateGameObject("Removed on contact"); object.GetTransform()->SetWorldPosition({0,0.4f,0});
    auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); body->SetUseGravity(false);
    object.AddComponent<ParticleCollider>(); scene.Play(); scene.SyncTransforms();
    const auto world=scene.GetPhysicsWorld(); const auto original=body->GetBodyHandle();
    bool delivered=false; Engine::Components::RigidbodyComponent* replacement=nullptr;
    scene.PhysicsContactEvent.AddListener([&](const Physics::ContactEvent& event)
    {
        if (delivered || event.phase!=Physics::ContactPhase::Enter) return;
        delivered=true; auto read=world->Read(); const auto revision=world->GetRevision();
        EXPECT_TRUE(scene.DestroyGameObject(object));
        auto& spawned=scene.CreateGameObject("Spawned in contact"); spawned.GetTransform()->SetWorldPosition({20,3,0});
        replacement=spawned.AddComponent<Engine::Components::RigidbodyComponent>(); spawned.AddComponent<ParticleCollider>();
        EXPECT_FALSE(replacement->GetBodyHandle().IsValid()); EXPECT_GT(scene.GetPendingPhysicsCommandCount(),0u);
        EXPECT_FALSE(scene.TrySyncTransforms()); EXPECT_TRUE(scene.SimulatePhysics(0.1f).empty());
        EXPECT_EQ(world->GetRevision(),revision);
        Physics::QueryHit hit; EXPECT_TRUE(read->Raycast({0,5,0},{0,-1,0},10,hit));
    });
    scene.FixedUpdate(1.0f/60); scene.SimulatePhysics(1.0f/60);
    ASSERT_TRUE(delivered); ASSERT_NE(replacement,nullptr); EXPECT_TRUE(world->IsValid(original));
    scene.SyncTransforms(); EXPECT_FALSE(world->IsValid(original)); EXPECT_TRUE(world->IsValid(replacement->GetBodyHandle()));
    EXPECT_EQ(world->GetStatistics().bodyCount,2u); EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u);
}

TEST(PhysicsStructureTests, RemovingAndReaddingRigidbodyReassemblesShapesWithFreshHandles)
{
    for (bool twoDimensional:{false,true})
    {
        Engine::SceneSystem::Scene scene; auto& object=scene.CreateGameObject("Ownership changes");
        const auto exercise=[&](auto* body,auto* collider,auto world)
        {
            scene.Play(); scene.SyncTransforms(); const auto original=body->GetBodyHandle();
            ASSERT_TRUE(object.RemoveComponent(body)); EXPECT_GT(scene.GetPendingPhysicsCommandCount(),0u);
            scene.SyncTransforms(); const auto standalone=collider->GetBodyHandle();
            EXPECT_NE(standalone,original); EXPECT_TRUE(world->IsValid(standalone)); EXPECT_FALSE(world->IsValid(original));
            using Body=std::remove_pointer_t<decltype(body)>;
            auto* replacement=object.AddComponent<Body>(); EXPECT_FALSE(replacement->GetBodyHandle().IsValid());
            scene.SyncTransforms(); EXPECT_EQ(collider->GetBodyHandle(),replacement->GetBodyHandle());
            EXPECT_FALSE(world->IsValid(standalone)); EXPECT_EQ(world->GetStatistics().bodyCount,1u); EXPECT_EQ(world->GetStatistics().shapeCount,1u);
        };
        if (twoDimensional)
        { auto* body=object.AddComponent<Engine::Components::Rigidbody2DComponent>(); auto* collider=object.AddComponent<Engine::Components::PhysicsCollider2DComponent>(); exercise(body,collider,scene.GetPhysicsWorld2D()); }
        else
        { auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); auto* collider=object.AddComponent<ParticleCollider>(); exercise(body,collider,scene.GetPhysicsWorld()); }
    }
}

TEST(PhysicsStructureTests, DisableThenReenableBeforeCommitCannotRemoveTheReplacementShape)
{
    for (bool twoDimensional:{false,true})
    {
        Engine::SceneSystem::Scene scene; auto& object=scene.CreateGameObject("Enable cycle");
        const auto exercise=[&](auto* body,auto* collider,auto world)
        {
            scene.Play(); scene.FixedUpdate(1.0f/60); scene.SimulatePhysics(1.0f/60);
            const auto original=collider->GetShapeHandle(); const auto nativeBody=body->GetBodyHandle();
            collider->SetEnabled(false); EXPECT_FALSE(collider->GetShapeHandle().IsValid());
            collider->SetEnabled(true); scene.SyncTransforms();
            EXPECT_EQ(body->GetBodyHandle(),nativeBody); EXPECT_NE(collider->GetShapeHandle(),original);
            EXPECT_TRUE(world->IsValid(collider->GetShapeHandle())); EXPECT_FALSE(world->IsValid(original));
            EXPECT_EQ(world->GetStatistics().shapeCount,1u); EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u);
            scene.Stop(); EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u);
        };
        if (twoDimensional)
        { auto* body=object.AddComponent<Engine::Components::Rigidbody2DComponent>(); auto* collider=object.AddComponent<Engine::Components::PhysicsCollider2DComponent>(); exercise(body,collider,scene.GetPhysicsWorld2D()); }
        else
        { auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); auto* collider=object.AddComponent<ParticleCollider>(); exercise(body,collider,scene.GetPhysicsWorld()); }
    }
}

TEST(PhysicsStructureTests, SceneDestructionFlushesPendingAndDuplicateReleasesInBothWorlds)
{
    auto scene=std::make_unique<Engine::SceneSystem::Scene>();
    auto& spatial=scene->CreateGameObject("Spatial"); auto* body=spatial.AddComponent<Engine::Components::RigidbodyComponent>(); spatial.AddComponent<ParticleCollider>();
    auto& planar=scene->CreateGameObject("Planar"); auto* body2D=planar.AddComponent<Engine::Components::Rigidbody2DComponent>(); planar.AddComponent<Engine::Components::PhysicsCollider2DComponent>();
    scene->Play(); scene->SyncTransforms(); const auto world=scene->GetPhysicsWorld(); const auto world2D=scene->GetPhysicsWorld2D();
    scene->QueuePhysicsRemoval(world,body->GetBodyHandle()); scene->QueuePhysicsRemoval(world2D,body2D->GetBodyHandle());
    EXPECT_EQ(scene->GetPendingPhysicsCommandCount(),2u); scene.reset();
    EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(world->GetStatistics().shapeCount,0u);
    EXPECT_EQ(world2D->GetStatistics().bodyCount,0u); EXPECT_EQ(world2D->GetStatistics().shapeCount,0u);
}

TEST(PhysicsStructureTests, RepeatedPlayStopClearsRetainedWorldsAndDoesNotReplayOldCommands)
{
    Engine::SceneSystem::Scene scene;
    auto& spatial=scene.CreateGameObject("Repeated spatial"); auto* body=spatial.AddComponent<Engine::Components::RigidbodyComponent>(); spatial.AddComponent<ParticleCollider>();
    auto& planar=scene.CreateGameObject("Repeated planar"); auto* body2D=planar.AddComponent<Engine::Components::Rigidbody2DComponent>(); planar.AddComponent<Engine::Components::PhysicsCollider2DComponent>();
    body->SetUseGravity(false); body2D->SetGravityScale(0);
    uint64_t oldWorld=0, oldWorld2D=0;
    for (int cycle=0;cycle<8;++cycle)
    {
        scene.Play(); scene.SyncTransforms();
        scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
        EXPECT_NEAR(body->GetVelocity().LengthSquared(),0,1e-6f); EXPECT_NEAR(body2D->GetVelocity().x,0,1e-6f);
        const auto world=scene.GetPhysicsWorld(); const auto world2D=scene.GetPhysicsWorld2D();
        const auto handle=body->GetBodyHandle(); const auto handle2D=body2D->GetBodyHandle();
        ASSERT_TRUE(world->IsValid(handle)); ASSERT_TRUE(world2D->IsValid(handle2D));
        EXPECT_NE(handle.world,oldWorld); EXPECT_NE(handle2D.world,oldWorld2D); oldWorld=handle.world; oldWorld2D=handle2D.world;
        scene.QueuePhysicsRemoval(world,handle); scene.QueuePhysicsRemoval(world2D,handle2D);
        body->AddImpulse({100,0,0}); body2D->AddImpulse({100,0});
        scene.Stop(); scene.Stop();
        EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(world2D->GetStatistics().bodyCount,0u);
        EXPECT_FALSE(body->GetBodyHandle().IsValid()); EXPECT_FALSE(body2D->GetBodyHandle().IsValid());
        EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u);
    }
}

TEST(PhysicsStructureTests, SceneCloneNeverCopiesPendingCommandsOrRuntimeHandles)
{
    Engine::SceneSystem::Scene scene; auto& object=scene.CreateGameObject("Clone with pending removal");
    auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); auto* collider=object.AddComponent<ParticleCollider>();
    scene.Play(); scene.SyncTransforms(); const auto original=body->GetBodyHandle();
    ASSERT_TRUE(object.RemoveComponent(collider)); ASSERT_GT(scene.GetPendingPhysicsCommandCount(),0u);
    auto clone=Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene));
    ASSERT_TRUE(clone); EXPECT_EQ(clone->GetPendingPhysicsCommandCount(),0u);
    auto* copied=clone->FindGameObjectByName("Clone with pending removal")->GetComponent<Engine::Components::RigidbodyComponent>();
    ASSERT_TRUE(copied); EXPECT_FALSE(copied->GetBodyHandle().IsValid());
    clone->Play(); clone->SyncTransforms(); EXPECT_NE(copied->GetBodyHandle().world,original.world);
    EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().shapeCount,1u);
    scene.SyncTransforms(); EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().shapeCount,0u);
    EXPECT_TRUE(clone->GetPhysicsWorld()->IsValid(copied->GetBodyHandle()));
}

TEST(PhysicsStructureTests, ExternallyRemovedShapesAreRebuiltInsteadOfAcceptingStaleCacheEntries)
{
    for (bool twoDimensional:{false,true})
    {
        Engine::SceneSystem::Scene scene; auto& object=scene.CreateGameObject("Stale shape");
        const auto exercise=[&](auto* collider,auto world)
        {
            scene.SyncTransforms(); const auto original=collider->GetShapeHandle(); ASSERT_TRUE(original.IsValid());
            ASSERT_TRUE(world->Remove(original)); EXPECT_FALSE(world->IsValid(original)); scene.SyncTransforms();
            EXPECT_NE(collider->GetShapeHandle(),original); EXPECT_TRUE(world->IsValid(collider->GetShapeHandle()));
            EXPECT_EQ(world->GetStatistics().bodyCount,1u); EXPECT_EQ(world->GetStatistics().shapeCount,1u);
        };
        if (twoDimensional) exercise(object.AddComponent<Engine::Components::PhysicsCollider2DComponent>(),scene.GetPhysicsWorld2D());
        else exercise(object.AddComponent<ParticleCollider>(),scene.GetPhysicsWorld());
    }
}

TEST(PhysicsPresentationTests, RenderAlphaMovesFrozenProxiesWithoutChangingAuthorityOrQueries)
{
    Engine::SceneSystem::Scene scene; auto settings=scene.GetPhysicsSettings(); settings.interpolate=true;
    std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error));
    auto& object=scene.CreateGameObject("Moving body");
    auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); body->SetUseGravity(false); body->SetVelocity({10,0,0});
    auto* collider=object.AddComponent<ParticleCollider>();
    auto& light=scene.CreateGameObject("Child light"); light.SetParent(object); light.GetTransform()->SetLocalPosition({0,2,0});
    light.AddComponent<Engine::Components::LightComponent>();
    scene.Play(); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    ASSERT_TRUE(body->GetBodyHandle().IsValid()); ASSERT_TRUE(collider->GetShapeHandle().IsValid());
    const auto authority=object.GetTransform()->GetWorldPosition(); ASSERT_NEAR(authority.x,1,1e-5f);
    scene.SetPhysicsInterpolationAlpha(0.5);
    const auto presented=scene.CapturePhysicsPresentationTransform(object);
    EXPECT_TRUE(presented.usesPhysicsPose); EXPECT_NEAR(presented.worldPosition.x,0.5f,1e-5f);
    Engine::Rendering::SceneProxySourceRegistry sources;
    const auto first=sources.Collect(scene,{},1); ASSERT_EQ(first.commands.size(),1u);
    EXPECT_NEAR(first.commands[0].transform.worldPosition.x,0.5f,1e-5f);
    EXPECT_NEAR(first.commands[0].transform.worldPosition.y,2,1e-5f);
    const auto renderRevision=scene.GetRenderContentRevision(), physicsRevision=scene.GetPhysicsRevision();
    scene.SetPhysicsInterpolationAlpha(0.75);
    EXPECT_GT(scene.GetRenderContentRevision(),renderRevision); EXPECT_EQ(scene.GetPhysicsRevision(),physicsRevision);
    const auto next=sources.Collect(scene,{},2); ASSERT_EQ(next.commands.size(),1u);
    EXPECT_EQ(next.commands[0].kind,Engine::Rendering::SceneProxyCommandKind::UpdateTransform);
    EXPECT_NEAR(next.commands[0].transform.worldPosition.x,0.75f,1e-5f);
    EXPECT_NEAR(first.commands[0].transform.worldPosition.x,0.5f,1e-5f);
    EXPECT_TRUE(sources.Collect(scene,{},3).Empty());
    collider->SetSize({0.6f,0.6f,0.6f}); scene.SyncTransforms();
    const auto rebuilt=sources.Collect(scene,{},4); ASSERT_EQ(rebuilt.commands.size(),1u);
    EXPECT_NEAR(rebuilt.commands[0].transform.worldPosition.x,1,1e-5f);
    EXPECT_EQ(object.GetTransform()->GetWorldPosition(),authority);
    Physics::BodyState native; ASSERT_TRUE(scene.GetPhysicsWorld()->GetState(body->GetBodyHandle(),native));
    EXPECT_EQ(native.position,authority);
    Physics::QueryHit hit; ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({1,3,0},{0,-1,0},10,hit));
    EXPECT_EQ(hit.body,body->GetBodyHandle());
}

TEST(PhysicsPresentationTests, TeleportsRestoresAndShapeRebuildsClearInterpolationHistory)
{
    Engine::SceneSystem::Scene scene; auto settings=scene.GetPhysicsSettings(); settings.interpolate=true;
    std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error));
    auto& object=scene.CreateGameObject("Discontinuities");
    auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); body->SetUseGravity(false); body->SetVelocity({10,0,0});
    auto* collider=object.AddComponent<ParticleCollider>();
    scene.Play(); scene.SimulatePhysics(0.1f); scene.SetPhysicsInterpolationAlpha(0.5);
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(object).worldPosition.x,0.5f,1e-5f);
    object.GetTransform()->SetWorldPosition({10,0,0});
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(object).worldPosition.x,10,1e-5f);
    scene.SyncTransforms();
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(object).worldPosition.x,10,1e-5f);
    scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(object).worldPosition.x,10.5f,1e-5f);
    collider->SetSize({1,1,1}); scene.SyncTransforms();
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(object).worldPosition.x,11,1e-5f);
    const auto snapshot=scene.GetPhysicsWorld()->CaptureState(); scene.SimulatePhysics(0.1f);
    ASSERT_TRUE(scene.GetPhysicsWorld()->RestoreState(snapshot,error));
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(object).worldPosition.x,11,1e-5f);
    scene.SyncTransforms(); EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x,11,1e-5f);
    body->Teleport({30,0,0},Maths::Quaternion::Identity,false); body->ApplyPendingCommands();
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(object).worldPosition.x,30,1e-5f);
    EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x,30,1e-5f);
    EXPECT_EQ(body->GetVelocity(),Maths::Vector3{});
}

TEST(PhysicsPresentationTests, NativeTeleportsAndRestoresInvalidateRetainedRenderProxiesWhilePaused)
{
    for (const bool twoDimensional : {false,true})
    {
        Engine::SceneSystem::Scene scene; auto settings=scene.GetPhysicsSettings(); settings.interpolate=true;
        std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error));
        auto& object=scene.CreateGameObject("Native state presentation"); object.AddComponent<Engine::Components::LightComponent>();
        Engine::Components::RigidbodyComponent* body3D=nullptr; Engine::Components::Rigidbody2DComponent* body2D=nullptr;
        if (twoDimensional)
        {
            body2D=object.AddComponent<Engine::Components::Rigidbody2DComponent>(); body2D->SetGravityScale(0); body2D->SetVelocity({10,0});
            object.AddComponent<Engine::Components::PhysicsCollider2DComponent>();
        }
        else
        {
            body3D=object.AddComponent<Engine::Components::RigidbodyComponent>(); body3D->SetUseGravity(false); body3D->SetVelocity({10,0,0});
            object.AddComponent<ParticleCollider>();
        }
        scene.Play(); scene.SimulatePhysics(0.1f); scene.SetPhysicsInterpolationAlpha(0.5);
        Engine::Rendering::SceneProxySourceRegistry sources;
        const auto first=sources.Collect(scene,{},1); ASSERT_EQ(first.commands.size(),1u);
        EXPECT_NEAR(first.commands[0].transform.worldPosition.x,0.5f,1e-5f);
        const auto exercise=[&](auto world, auto handle, auto position, auto rotation)
        {
            Engine::Rendering::RenderScene renderScene; Engine::Rendering::RenderSceneSyncOptions options;
            options.trustSceneRenderContentRevision=true;
            Tests::SynchronizeSceneForTest(renderScene,scene,options);
            const auto nativeRevision=world->GetRevision(), sceneRevision=scene.GetRenderContentRevision();
            const auto snapshot=world->CaptureState();
            EXPECT_EQ(world->GetRevision(),nativeRevision);
            ASSERT_TRUE(world->SetTransform(handle,position,rotation));
            const auto moved=sources.Collect(scene,{},2); ASSERT_EQ(moved.commands.size(),1u);
            EXPECT_NEAR(moved.commands[0].transform.worldPosition.x,5,1e-5f);
            EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x,1,1e-5f);
            auto movedCapture=renderScene.BeginSynchronize(scene,options); ASSERT_TRUE(movedCapture.IsValid());
            EXPECT_TRUE(movedCapture.physicsMutationObserved); EXPECT_TRUE(movedCapture.sourceNeedsCollection);
            EXPECT_FALSE(movedCapture.canReuseProxySynchronization); ASSERT_EQ(movedCapture.proxyBatch.commands.size(),1u);
            EXPECT_NEAR(movedCapture.proxyBatch.commands[0].transform.worldPosition.x,5,1e-5f);
            Tests::CompleteSceneSynchronizationForTest(renderScene,std::move(movedCapture),options);
            ASSERT_TRUE(world->RestoreState(snapshot,error)) << error;
            const auto restored=sources.Collect(scene,{},3); ASSERT_EQ(restored.commands.size(),1u);
            EXPECT_NEAR(restored.commands[0].transform.worldPosition.x,1,1e-5f);
            EXPECT_EQ(scene.GetRenderContentRevision(),sceneRevision);
            auto restoredCapture=renderScene.BeginSynchronize(scene,options); ASSERT_TRUE(restoredCapture.IsValid());
            EXPECT_TRUE(restoredCapture.physicsMutationObserved); ASSERT_EQ(restoredCapture.proxyBatch.commands.size(),1u);
            EXPECT_NEAR(restoredCapture.proxyBatch.commands[0].transform.worldPosition.x,1,1e-5f);
            Tests::CompleteSceneSynchronizationForTest(renderScene,std::move(restoredCapture),options);
            auto stable=renderScene.BeginSynchronize(scene,options); ASSERT_TRUE(stable.IsValid());
            EXPECT_TRUE(stable.canReuseProxySynchronization); EXPECT_FALSE(stable.sourceNeedsCollection);
            Tests::CompleteSceneSynchronizationForTest(renderScene,std::move(stable),options);
            const auto scans=sources.GetCollectionScanCountForTesting(), revision=world->GetRevision();
            EXPECT_TRUE(sources.Collect(scene,{},4).Empty());
            EXPECT_EQ(sources.GetCollectionScanCountForTesting(),scans); EXPECT_EQ(world->GetRevision(),revision);
        };
        if (twoDimensional) exercise(scene.GetPhysicsWorld2D(),body2D->GetBodyHandle(),Maths::Vector2{5,0},0.0f);
        else exercise(scene.GetPhysicsWorld(),body3D->GetBodyHandle(),Maths::Vector3{5,0,0},Maths::Quaternion::Identity);
    }
}

TEST(PhysicsPresentationTests, NestedBodiesRemainIndependentRegardlessOfCreationOrder)
{
    Engine::SceneSystem::Scene scene; auto settings=scene.GetPhysicsSettings(); settings.interpolate=true;
    std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error));
    auto& child=scene.CreateGameObject("Created first"); auto& parent=scene.CreateGameObject("Created second");
    child.SetParent(parent); child.GetTransform()->SetLocalPosition({0,5,0});
    auto* childBody=child.AddComponent<Engine::Components::RigidbodyComponent>(); childBody->SetUseGravity(false); childBody->SetVelocity({0,0,2});
    child.AddComponent<ParticleCollider>();
    auto* parentBody=parent.AddComponent<Engine::Components::RigidbodyComponent>(); parentBody->SetUseGravity(false); parentBody->SetVelocity({10,0,0});
    parent.AddComponent<ParticleCollider>();
    scene.Play(); scene.SimulatePhysics(0.1f); scene.SetPhysicsInterpolationAlpha(0.5);
    EXPECT_NEAR(parent.GetTransform()->GetWorldPosition().x,1,1e-5f);
    EXPECT_NEAR(child.GetTransform()->GetWorldPosition().x,0,1e-5f);
    EXPECT_NEAR(child.GetTransform()->GetWorldPosition().z,0.2f,1e-5f);
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(child).worldPosition.x,0,1e-5f);
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(child).worldPosition.z,0.1f,1e-5f);
    scene.SyncTransforms();
    EXPECT_NEAR(child.GetTransform()->GetWorldPosition().x,0,1e-5f);
    scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(child.GetTransform()->GetWorldPosition().x,0,1e-5f);
    EXPECT_NEAR(child.GetTransform()->GetWorldPosition().z,0.4f,1e-5f);
}

TEST(PhysicsPresentationTests, ParentMotionWithoutAFixedStepCannotDragDynamicBody)
{
    Engine::SceneSystem::Scene scene; auto& parent=scene.CreateGameObject("Parent");
    auto& child=scene.CreateGameObject("Body"); child.SetParent(parent); child.GetTransform()->SetLocalPosition({1,2,3});
    auto* body=child.AddComponent<Engine::Components::RigidbodyComponent>(); body->SetUseGravity(false); child.AddComponent<ParticleCollider>();
    scene.Play(); scene.SyncTransforms();
    parent.GetTransform()->SetWorldPosition({10,0,0}); scene.UpdateGameplay(0);
    EXPECT_EQ(child.GetTransform()->GetWorldPosition(),Maths::Vector3(1,2,3));
    Physics::BodyState state; ASSERT_TRUE(scene.GetPhysicsWorld()->GetState(body->GetBodyHandle(),state));
    EXPECT_EQ(state.position,Maths::Vector3(1,2,3));
}

TEST(PhysicsPresentationTests, CameraCapturesSmoothedViewAndKeepsAuthoringCameraSemantics)
{
    Engine::SceneSystem::Scene scene; auto settings=scene.GetPhysicsSettings(); settings.interpolate=true;
    std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error));
    auto& object=scene.CreateGameObject("Camera rig");
    auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>(); body->SetUseGravity(false); body->SetVelocity({10,0,0});
    object.AddComponent<ParticleCollider>();
    auto& child=scene.CreateGameObject("Camera"); child.SetParent(object); child.GetTransform()->SetLocalPosition({0,2,3});
    auto* camera=child.AddComponent<Engine::Components::CameraComponent>(); camera->SetFov(73); camera->SetVisibleLayers(Engine::LayerMask{4});
    scene.Play(); scene.SimulatePhysics(0.1f); scene.SetPhysicsInterpolationAlpha(0.25);
    auto* captured=camera->GetRenderCamera(); ASSERT_NE(captured,camera->GetCamera()); captured->CacheMatrices(640,480);
    EXPECT_NEAR(captured->GetPosition().x,0.25f,1e-5f); EXPECT_NEAR(captured->GetPosition().y,2,1e-5f);
    EXPECT_NEAR(camera->GetCamera()->GetPosition().x,1,1e-5f); EXPECT_NEAR(child.GetTransform()->GetWorldPosition().x,1,1e-5f);
    EXPECT_FLOAT_EQ(captured->GetFov(),73); EXPECT_EQ(captured->GetVisibleLayerMask(),4u);
    camera->GetCamera()->SetPosition({20,2,3});
    EXPECT_EQ(child.GetTransform()->GetWorldPosition(),Maths::Vector3(20,2,3));
    settings.interpolate=false; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error));
    EXPECT_EQ(camera->GetRenderCamera(),camera->GetCamera());
    EXPECT_FALSE(scene.CapturePhysicsPresentationTransform(object).usesPhysicsPose);
}

TEST(PhysicsSceneTests, ColliderGeometryFollowsWorldScale)
{
    Engine::SceneSystem::Scene scene;
    if (scene.GetPhysicsWorld() == nullptr)
        GTEST_SKIP() << "PhysX query world is unavailable";

    auto& object = scene.CreateGameObject("Scaled box");
    auto* collider = object.AddComponent<ParticleCollider>();
    ASSERT_NE(collider, nullptr);
    collider->SetSize({0.5f, 0.5f, 0.5f});
    object.GetTransform()->SetWorldScale({2, 2, 2});
    scene.Update(0);

    Physics::QueryHit hit;
    ASSERT_TRUE(scene.GetPhysicsWorld()->Read()->Raycast({0, 4, 0}, {0, -1, 0}, 10, hit));
    EXPECT_NEAR(hit.position.y, 1.0f, 0.01f);
}

TEST(PhysicsSceneTests, CapsuleUsesYUpAxisAndFollowsRotation)
{
    Engine::SceneSystem::Scene scene;
    if (scene.GetPhysicsWorld() == nullptr)
        GTEST_SKIP() << "PhysX query world is unavailable";

    auto& object = scene.CreateGameObject("Capsule");
    auto* collider = object.AddComponent<ParticleCollider>();
    ASSERT_NE(collider, nullptr);
    collider->SetShape(ParticleCollider::EColliderShape::CAPSULE);
    collider->SetRadius(0.5f);
    collider->SetHalfHeight(1.0f);
    scene.Update(0);

    const auto& world = scene.GetPhysicsWorld();
    Physics::QueryHit hit;
    ASSERT_TRUE(world->Read()->Raycast({0, 4, 0}, {0, -1, 0}, 10, hit));
    EXPECT_NEAR(hit.position.y, 1.5f, 0.02f);

    // A quarter turn around Z maps the Y axis onto X, so the capsule now lies
    // along X while the default ray still lands on its radius.
    object.GetTransform()->SetWorldRotation({0.0f, 0.0f, std::sqrt(0.5f), std::sqrt(0.5f)});
    scene.Update(0);
    ASSERT_TRUE(world->Read()->Raycast({4, 0, 0}, {-1, 0, 0}, 10, hit));
    EXPECT_NEAR(hit.position.x, 1.5f, 0.02f);
    ASSERT_TRUE(world->Read()->Raycast({0, 4, 0}, {0, -1, 0}, 10, hit));
    EXPECT_NEAR(hit.position.y, 0.5f, 0.02f);
}

TEST(PhysicsSceneTests, RemovingOrDestroyingCollidersReleasesQueryBodies)
{
    Engine::SceneSystem::Scene scene;
    if (scene.GetPhysicsWorld() == nullptr)
        GTEST_SKIP() << "PhysX query world is unavailable";

    const auto& world = scene.GetPhysicsWorld();
    Physics::QueryHit hit;
    auto& object = scene.CreateGameObject("Sphere");
    auto* collider = object.AddComponent<ParticleCollider>();
    ASSERT_NE(collider, nullptr);
    collider->SetShape(ParticleCollider::EColliderShape::SPHERE);
    collider->SetRadius(1.0f);
    scene.Update(0);
    ASSERT_TRUE(world->Read()->Raycast({0, 3, 0}, {0, -1, 0}, 10, hit));
    EXPECT_NEAR(hit.position.y, 1.0f, 0.01f);

    const auto revisionBeforeRemoval=scene.GetPhysicsRevision();
    ASSERT_TRUE(object.RemoveComponent(collider));
    scene.Update(0);
    EXPECT_GT(scene.GetPhysicsRevision(),revisionBeforeRemoval);
    EXPECT_FALSE(world->Read()->Raycast({0, 3, 0}, {0, -1, 0}, 10, hit));
    EXPECT_TRUE(scene.GetFastAccessComponents().physicsColliders.empty());

    auto* replacement = object.AddComponent<ParticleCollider>();
    ASSERT_NE(replacement, nullptr);
    replacement->SetShape(ParticleCollider::EColliderShape::SPHERE);
    replacement->SetRadius(1.0f);
    scene.Update(0);
    ASSERT_TRUE(world->Read()->Raycast({0, 3, 0}, {0, -1, 0}, 10, hit));
    scene.DestroyGameObject(object);
    scene.Update(0);
    EXPECT_FALSE(world->Read()->Raycast({0, 3, 0}, {0, -1, 0}, 10, hit));
}

TEST(PhysicsSceneTests, ParticlesCollideWithSceneCollidersWithoutManualWiring)
{
    Engine::SceneSystem::Scene scene;
    if (scene.GetPhysicsWorld() == nullptr)
        GTEST_SKIP() << "PhysX query world is unavailable";

    auto& floor = scene.CreateGameObject("Floor");
    auto* collider = floor.AddComponent<ParticleCollider>();
    ASSERT_NE(collider, nullptr);
    collider->SetSize({5, 0.5f, 5});
    floor.GetTransform()->SetWorldPosition({0, -0.5f, 0});

    const auto program = MakeCollidingEmitter();
    ASSERT_NE(program, nullptr);
    auto& effect = scene.CreateGameObject("Effect");
    auto* particles = effect.AddComponent<Engine::Components::ParticleSystemComponent>();
    ASSERT_NE(particles, nullptr);
    std::string error;
    ASSERT_TRUE(particles->SetCompiledSystem(program, error)) << error;
    particles->OnStart();
    scene.Update(0.2f);

    const auto frame = particles->CaptureFrame();
    ASSERT_NE(frame, nullptr) << particles->LastError();
    ASSERT_EQ(frame->cpu.size(), 1u);
    ASSERT_EQ(frame->cpu[0]->Count(), 1u);
    const auto velocity = frame->cpu[0]->FindAttribute("Particles.Velocity");
    const auto position = frame->cpu[0]->FindAttribute("Particles.Position");
    ASSERT_GE(velocity, 0);
    ASSERT_GE(position, 0);
    EXPECT_GT(frame->cpu[0]->Read(velocity, 0)[1], 0.0f);
    EXPECT_GE(frame->cpu[0]->Read(position, 0)[1], 0.0f);
}

TEST(PhysicsSceneTests, SceneRoundTripRestoresColliderGeometry)
{
    Engine::SceneSystem::Scene scene;
    if (scene.GetPhysicsWorld() == nullptr)
        GTEST_SKIP() << "PhysX query world is unavailable";

    auto& object = scene.CreateGameObject("Trigger volume");
    auto* collider = object.AddComponent<ParticleCollider>();
    ASSERT_NE(collider, nullptr);
    collider->SetSize({1, 2, 3});
    collider->SetLayers(4u);
    collider->SetTrigger(true);
    object.GetTransform()->SetWorldPosition({1, 2, 3});
    scene.Update(0);

    const auto document = Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene);
    ASSERT_FALSE(document.Validate().HasErrors());
    auto loaded = Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loaded, nullptr);
    auto* restoredObject = loaded->FindGameObjectByName("Trigger volume");
    ASSERT_NE(restoredObject, nullptr);
    auto* restored = restoredObject->GetComponent<ParticleCollider>();
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->GetShape(), ParticleCollider::EColliderShape::BOX);
    EXPECT_FLOAT_EQ(restored->GetSize().x, 1.0f);
    EXPECT_FLOAT_EQ(restored->GetSize().y, 2.0f);
    EXPECT_FLOAT_EQ(restored->GetSize().z, 3.0f);
    EXPECT_EQ(restored->GetLayers(), 4u);
    EXPECT_TRUE(restored->IsTrigger());

    loaded->Update(1.0f / 60);
    const auto& world = loaded->GetPhysicsWorld();
    ASSERT_NE(world, nullptr);
    Physics::QueryHit hit;
    // Trigger bodies stay out of default queries but remain available to
    // queries that opt in, which is how particle collision consumes them.
    EXPECT_FALSE(world->Read()->Raycast({1, 10, 3}, {0, -1, 0}, 20, hit));
    Physics::QueryFilter filter;
    filter.includeTriggers = true;
    ASSERT_TRUE(world->Read(filter)->Raycast({1, 10, 3}, {0, -1, 0}, 20, hit));
    EXPECT_NEAR(hit.position.y, 4.0f, 0.01f);
}

TEST(PhysicsSceneTests, DescendantCollidersShareTheNearestRigidbodyAndNestedBodiesStopOwnership)
{
    Engine::SceneSystem::Scene scene;
    auto& root=scene.CreateGameObject("Root"); auto* body=root.AddComponent<Engine::Components::RigidbodyComponent>();
    auto* own=root.AddComponent<ParticleCollider>();
    auto& child=scene.CreateGameObject("Child"); child.SetParent(root); child.GetTransform()->SetLocalPosition({2,0,0});
    auto* attached=child.AddComponent<ParticleCollider>();
    auto& nested=scene.CreateGameObject("Nested"); nested.SetParent(root); nested.GetTransform()->SetLocalPosition({5,0,0});
    auto* nestedBody=nested.AddComponent<Engine::Components::RigidbodyComponent>(); auto* nestedCollider=nested.AddComponent<ParticleCollider>();
    scene.SyncTransforms(); ASSERT_TRUE(scene.PeekPhysicsWorld());
    EXPECT_EQ(own->GetBodyHandle(),body->GetBodyHandle()); EXPECT_EQ(attached->GetBodyHandle(),body->GetBodyHandle());
    EXPECT_EQ(nestedCollider->GetBodyHandle(),nestedBody->GetBodyHandle()); EXPECT_NE(nestedBody->GetBodyHandle(),body->GetBodyHandle());
    EXPECT_EQ(scene.PeekPhysicsWorld()->GetStatistics().bodyCount,2u); EXPECT_EQ(scene.PeekPhysicsWorld()->GetStatistics().shapeCount,3u);
    Physics::QueryHit hit; ASSERT_TRUE(scene.PeekPhysicsWorld()->Read()->Raycast({2,3,0},{0,-1,0},5,hit));
    EXPECT_EQ(hit.owner,static_cast<uint64_t>(attached->GetInstanceID())); EXPECT_EQ(hit.shape,attached->GetShapeHandle());
}

TEST(PhysicsSceneTests, CompoundForceCommandsApplyOnceAndShapeRebuildPreservesMotion)
{
    Engine::SceneSystem::Scene scene;
    auto& root=scene.CreateGameObject("Body"); auto* body=root.AddComponent<Engine::Components::RigidbodyComponent>();
    body->SetUseGravity(false); body->SetMass(4);
    auto& child=scene.CreateGameObject("Shape"); child.SetParent(root); auto* collider=child.AddComponent<ParticleCollider>();
    scene.Play(); body->AddImpulse({4,0,0}); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(body->GetVelocity().x,1,1e-5f); EXPECT_NEAR(root.GetTransform()->GetWorldPosition().x,0.1f,1e-5f);
    const auto handle=body->GetBodyHandle(); collider->SetSize({1,0.5f,0.5f});
    scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_EQ(body->GetBodyHandle(),handle); EXPECT_NEAR(body->GetVelocity().x,1,1e-5f); EXPECT_NEAR(root.GetTransform()->GetWorldPosition().x,0.2f,1e-5f);
}

TEST(PhysicsSceneTests, DynamicDescendantKeepsWorldPoseWhenItsParentMoves)
{
    Engine::SceneSystem::Scene scene;
    auto& parent=scene.CreateGameObject("Parent"); auto* parentBody=parent.AddComponent<Engine::Components::RigidbodyComponent>(); parentBody->SetUseGravity(false);
    auto& child=scene.CreateGameObject("Child"); child.SetParent(parent); child.GetTransform()->SetLocalPosition({3,0,0});
    auto* childBody=child.AddComponent<Engine::Components::RigidbodyComponent>(); childBody->SetUseGravity(false); child.AddComponent<ParticleCollider>();
    scene.Play(); scene.FixedUpdate(0.02f); scene.SimulatePhysics(0.02f);
    parent.GetTransform()->SetWorldPosition({10,0,0});
    scene.FixedUpdate(0.02f); scene.SimulatePhysics(0.02f);
    EXPECT_NEAR(parent.GetTransform()->GetWorldPosition().x,10,1e-5f); EXPECT_NEAR(child.GetTransform()->GetWorldPosition().x,3,1e-5f);
    EXPECT_NEAR(child.GetTransform()->GetLocalPosition().x,-7,1e-5f);
}

TEST(PhysicsSceneTests, ExplicitTransformEditTeleportsOnceAndQueuedTeleportCanClearVelocity)
{
    Engine::SceneSystem::Scene scene;
    auto& object=scene.CreateGameObject("Dynamic"); auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>();
    body->SetUseGravity(false); object.AddComponent<ParticleCollider>(); scene.Play();
    body->SetVelocity({2,0,0}); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    object.GetTransform()->SetWorldPosition({5,0,0}); scene.SyncTransforms();
    Physics::BodyState state; ASSERT_TRUE(scene.PeekPhysicsWorld()->GetState(body->GetBodyHandle(),state));
    EXPECT_NEAR(state.position.x,5,1e-5f); EXPECT_NEAR(state.velocity.x,2,1e-5f);
    scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f); EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x,5.2f,1e-5f);
    body->Teleport({10,0,0},Maths::Quaternion::Identity,false); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x,10,1e-5f); EXPECT_NEAR(body->GetVelocity().LengthSquared(),0,1e-5f);
}

TEST(PhysicsSceneTests, StaticMirrorsBakeCenterAndShearReportsADiagnostic)
{
    Engine::SceneSystem::Scene scene;
    auto& object=scene.CreateGameObject("Mirrored"); object.GetTransform()->SetLocalScale({-2,1,1});
    auto* collider=object.AddComponent<ParticleCollider>(); collider->SetCenter({1,0,0}); scene.SyncTransforms();
    Physics::QueryHit hit; ASSERT_TRUE(scene.PeekPhysicsWorld()->Read()->Raycast({-2,3,0},{0,-1,0},5,hit));
    EXPECT_EQ(hit.owner,static_cast<uint64_t>(collider->GetInstanceID()));
    auto& child=scene.CreateGameObject("Sheared"); child.SetParent(object);
    child.GetTransform()->SetLocalRotation({0,0,std::sqrt(0.5f-0.35355339f),std::sqrt(0.5f+0.35355339f)});
    auto* invalid=child.AddComponent<ParticleCollider>(); scene.SyncTransforms();
    EXPECT_FALSE(invalid->IsQueryReady()); EXPECT_FALSE(invalid->LastError().empty()); EXPECT_TRUE(collider->IsQueryReady());
}

TEST(PhysicsSceneTests, StopAndComponentRemovalReleaseBodiesEvenWhenAReadWorldIsRetained)
{
    Engine::SceneSystem::Scene scene;
    auto& object=scene.CreateGameObject("Compound"); auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>();
    auto* first=object.AddComponent<ParticleCollider>(); auto* second=object.AddComponent<ParticleCollider>(); second->SetCenter({2,0,0});
    scene.Play(); scene.SyncTransforms(); const auto world=scene.PeekPhysicsWorld(); ASSERT_TRUE(world);
    EXPECT_EQ(world->GetStatistics().bodyCount,1u); EXPECT_EQ(world->GetStatistics().shapeCount,2u);
    ASSERT_TRUE(object.RemoveComponent(first)); scene.SyncTransforms(); EXPECT_TRUE(world->IsValid(body->GetBodyHandle())); EXPECT_EQ(world->GetStatistics().shapeCount,1u);
    scene.Stop(); EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(world->GetStatistics().shapeCount,0u);
}

TEST(PhysicsSceneTests, CylinderAuthoringUsesYUpDimensionsAndSurvivesSceneRoundTrip)
{
    Engine::SceneSystem::Scene scene;
    auto& object=scene.CreateGameObject("Cylinder"); auto* collider=object.AddComponent<ParticleCollider>();
    collider->SetShape(ParticleCollider::EColliderShape::CYLINDER); collider->SetRadius(0.5f); collider->SetHalfHeight(1);
    object.GetTransform()->SetLocalScale({2,3,4}); scene.SyncTransforms();
    Physics::QueryHit hit; ASSERT_TRUE(scene.PeekPhysicsWorld()->Read()->Raycast({0,6,0},{0,-1,0},10,hit));
    EXPECT_NEAR(hit.position.y,3,0.01f);
    auto loaded=Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene));
    ASSERT_TRUE(loaded); loaded->SyncTransforms();
    auto* restored=loaded->FindGameObjectByName("Cylinder")->GetComponent<ParticleCollider>(); ASSERT_TRUE(restored);
    EXPECT_EQ(restored->GetShape(),ParticleCollider::EColliderShape::CYLINDER);
    ASSERT_TRUE(loaded->PeekPhysicsWorld()->Read()->Raycast({0,6,0},{0,-1,0},10,hit)); EXPECT_NEAR(hit.position.y,3,0.01f);
}

TEST(PhysicsSceneTests, FixedUpdateMassChangesPreservePreviouslySubmittedForceAndBodyIdentity)
{
    Engine::SceneSystem::Scene scene;
    auto& object=scene.CreateGameObject("Body"); auto* body=object.AddComponent<Engine::Components::RigidbodyComponent>();
    body->SetUseGravity(false); object.AddComponent<ParticleCollider>(); scene.Play(); scene.SyncTransforms();
    const auto handle=body->GetBodyHandle();
    body->AddForce({10,0,0}); scene.FixedUpdate(0.1f); body->SetMass(2); scene.SimulatePhysics(0.1f);
    EXPECT_EQ(body->GetBodyHandle(),handle);
    const float committedVelocity=body->GetVelocity().x; EXPECT_GT(committedVelocity,0);
    scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f); EXPECT_NEAR(body->GetVelocity().x,committedVelocity,1e-5f);
}

TEST(PhysicsSceneTests, WorldSettingsApplyToBothSolversWithoutRebuildingBodyHandles)
{
    Engine::SceneSystem::Scene scene;
    auto settings=scene.GetPhysicsSettings(); settings.gravity3D={1,-2,3}; settings.gravity2D={-4,-5}; settings.solverSubsteps2D=2;
    std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error)) << error;
    auto& spatial=scene.CreateGameObject("Spatial"); auto* body=spatial.AddComponent<Engine::Components::RigidbodyComponent>(); spatial.AddComponent<ParticleCollider>();
    auto& planar=scene.CreateGameObject("Planar"); auto* body2D=planar.AddComponent<Engine::Components::Rigidbody2DComponent>(); planar.AddComponent<Engine::Components::PhysicsCollider2DComponent>();
    scene.Play(); scene.SyncTransforms(); const auto original=body->GetBodyHandle(); const auto original2D=body2D->GetBodyHandle();
    ASSERT_EQ(scene.GetPhysicsWorld2D()->GetSolverSubsteps(),2u);
    scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(body->GetVelocity().x,0.1f,1e-5f); EXPECT_NEAR(body->GetVelocity().y,-0.2f,1e-5f); EXPECT_NEAR(body->GetVelocity().z,0.3f,1e-5f);
    EXPECT_NEAR(body2D->GetVelocity().x,-0.4f,1e-5f); EXPECT_NEAR(body2D->GetVelocity().y,-0.5f,1e-5f);
    settings.gravity3D={0,0,0}; settings.gravity2D={0,0}; settings.solverSubsteps2D=8; settings.workerCount3D=1; settings.workerCount2D=1;
    ASSERT_TRUE(scene.SetPhysicsSettings(settings,error)) << error;
    EXPECT_EQ(body->GetBodyHandle(),original); EXPECT_EQ(body2D->GetBodyHandle(),original2D);
    EXPECT_TRUE(scene.HasPendingPhysicsWorkerSettings()); EXPECT_EQ(scene.GetPhysicsWorld2D()->GetSolverSubsteps(),8u);
    scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(body->GetVelocity().y,-0.2f,1e-5f); EXPECT_NEAR(body2D->GetVelocity().y,-0.5f,1e-5f);
    scene.Stop(); scene.GetPhysicsWorld(); scene.GetPhysicsWorld2D(); EXPECT_FALSE(scene.HasPendingPhysicsWorkerSettings());
}

TEST(PhysicsSceneTests, SceneWorldSettingsSurviveTextRoundTripAndCloning)
{
    Engine::SceneSystem::Scene scene;
    auto settings=scene.GetPhysicsSettings(); settings.gravity3D={0,-3,0}; settings.gravity2D={2,-6};
    settings.solverSubsteps2D=3; settings.workerCount3D=1; settings.workerCount2D=1; settings.interpolate=true;
    std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error));
    auto& object=scene.CreateGameObject("Body"); object.AddComponent<Engine::Components::Rigidbody2DComponent>(); object.AddComponent<Engine::Components::PhysicsCollider2DComponent>();
    const auto document=Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene);
    const auto decoded=Engine::Serialize::ObjectGraphReader::Read(Engine::Serialize::ObjectGraphWriter::Write(document)); ASSERT_TRUE(decoded);
    auto clone=Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(*decoded); ASSERT_TRUE(clone);
    EXPECT_TRUE(clone->GetPhysicsSettings()==settings); clone->SyncTransforms(); EXPECT_EQ(clone->GetPhysicsWorld2D()->GetSolverSubsteps(),3u);
    clone->Play(); clone->FixedUpdate(0.1f); clone->SimulatePhysics(0.1f);
    auto* restored=clone->FindGameObjectByName("Body")->GetComponent<Engine::Components::Rigidbody2DComponent>(); ASSERT_TRUE(restored);
    EXPECT_NEAR(restored->GetVelocity().x,0.2f,1e-5f); EXPECT_NEAR(restored->GetVelocity().y,-0.6f,1e-5f);
    EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().stepIndex,0u);
}

TEST(PhysicsSceneTests, InvalidWorldSettingsLeaveConfigurationAndNativeWorldsUnchanged)
{
    Engine::SceneSystem::Scene scene; scene.GetPhysicsWorld(); scene.GetPhysicsWorld2D();
    const auto original=scene.GetPhysicsSettings(); const auto revision=scene.GetPhysicsRevision(); std::string error;
    auto invalid=original; invalid.gravity3D.x=std::numeric_limits<float>::quiet_NaN(); EXPECT_FALSE(scene.SetPhysicsSettings(invalid,error));
    invalid=original; invalid.solverSubsteps2D=0; EXPECT_FALSE(scene.SetPhysicsSettings(invalid,error));
    invalid.solverSubsteps2D=65; EXPECT_FALSE(scene.SetPhysicsSettings(invalid,error));
    invalid=original; invalid.workerCount2D=32; EXPECT_FALSE(scene.SetPhysicsSettings(invalid,error));
    EXPECT_TRUE(scene.GetPhysicsSettings()==original); EXPECT_EQ(scene.GetPhysicsRevision(),revision); EXPECT_EQ(scene.GetPhysicsWorld2D()->GetSolverSubsteps(),4u);
}

TEST(PhysicsSceneTests, OldSceneWithoutPhysicsSettingsUsesDefaultsWithoutMutatingItsDocument)
{
    using namespace Engine::Serialize;
    Engine::SceneSystem::Scene scene; auto document=ObjectGraphSerializer::SerializeScene(scene); document.version=1;
    for(auto& record:document.objects) if(record.id==document.root) std::erase_if(record.properties,[](const auto& p){return p.name=="physics";});
    const auto text=ObjectGraphWriter::Write(document);
    auto loaded=ObjectGraphInstantiator::InstantiateScene(document); ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->GetPhysicsSettings()==Engine::SceneSystem::ScenePhysicsSettings{});
    EXPECT_EQ(ObjectGraphWriter::Write(document),text);
}

TEST(PhysicsSceneTests, MalformedPhysicsSettingsAreRejectedBeforeSceneInstantiation)
{
    using namespace Engine::Serialize;
    Engine::SceneSystem::Scene scene; const auto original=ObjectGraphSerializer::SerializeScene(scene);
    const std::vector<PropertyValue> invalid{
        PropertyValue::String("bad"), PropertyValue::Object({{"version",PropertyValue::Integer(2)}}),
        PropertyValue::Object({{"solverSubsteps2D",PropertyValue::Integer(0)}}),
        PropertyValue::Object({{"workerCount3D",PropertyValue::Integer(-1)}}),
        PropertyValue::Object({{"interpolate",PropertyValue::Integer(1)}}),
        PropertyValue::Object({{"gravity2D",PropertyValue::Array({PropertyValue::Number(0)})}}),
        PropertyValue::Object({{"gravity3D",PropertyValue::Array({PropertyValue::Number(0),PropertyValue::Number(0),PropertyValue::Number(std::numeric_limits<double>::infinity())})}}),
        PropertyValue::Object({{"solverSubsteps2D",PropertyValue::Integer(2)},{"solverSubsteps2D",PropertyValue::Integer(3)}})
    };
    for(const auto& value:invalid)
    {
        auto document=original;
        for(auto& record:document.objects) if(record.id==document.root) for(auto& property:record.properties) if(property.name=="physics") property.value=value;
        auto result=ObjectGraphInstantiator::InstantiateScene(document,LoadPolicy{});
        EXPECT_FALSE(result.scene); EXPECT_TRUE(result.diagnostics.HasErrors()); EXPECT_FALSE(ObjectGraphInstantiator::InstantiateScene(document));
    }
}
