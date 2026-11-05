#include <gtest/gtest.h>
#include <Components/PhysicsColliderComponent.h>
#include <Components/PhysicsCollider2DComponent.h>
#include <Components/RigidbodyComponent.h>
#include <Components/Rigidbody2DComponent.h>
#include <Components/TransformComponent.h>
#include <SceneSystem/SceneManager.h>
#include <Serialize/ObjectGraphInstantiator.h>
#include <Serialize/ObjectGraphSerializer.h>
#include <filesystem>
#include <type_traits>
#include <functional>

namespace
{
using namespace NLS;
using Engine::SceneSystem::Scene;
using Engine::SceneSystem::SceneManager;
namespace Components=Engine::Components;
namespace Serialize=Engine::Serialize;

struct CleanupProbe2D : Components::PhysicsCollider2DComponent
{
    int* destroys=nullptr;
    void OnDestroy() override { if(destroys) ++*destroys; Components::PhysicsCollider2DComponent::OnDestroy(); }
};
struct DestroyObserver : Components::Component
{
    std::function<void()> callback;
    void OnDestroy() override { if(callback) callback(); }
};

void AddBothDimensions(Scene& scene, const std::string& name)
{
    auto& spatial=scene.CreateGameObject(name);
    spatial.AddComponent<Components::RigidbodyComponent>()->SetUseGravity(false);
    spatial.AddComponent<Components::PhysicsColliderComponent>();
    auto& planar=scene.CreateGameObject(name+" 2D");
    planar.AddComponent<Components::Rigidbody2DComponent>()->SetGravityScale(0);
    planar.AddComponent<Components::PhysicsCollider2DComponent>();
}

TEST(PhysicsLifecycleTests, StoppedEditGeometryUsesDeferredOwnerCleanupInBothWorldsWithoutReplayingDestroy)
{
    Scene scene; auto& object=scene.CreateGameObject("Query geometry"); object.AddComponent<Components::PhysicsColliderComponent>();
    int destroys=0; auto* planar=object.AddComponent<CleanupProbe2D>(); planar->destroys=&destroys;
    planar->SetShape(Components::PhysicsCollider2DComponent::EColliderShape::POLYGON); planar->SetVertices({{0,0},{4,0},{4,1},{1,1},{1,4},{0,4}});
    scene.Play(); scene.SyncTransforms(); scene.Stop(); EXPECT_EQ(destroys,1); scene.SyncTransforms();
    auto world=scene.GetPhysicsWorld(); auto flat=scene.GetPhysicsWorld2D(); EXPECT_EQ(world->GetStatistics().bodyCount,1u); EXPECT_EQ(flat->GetStatistics().bodyCount,1u);
    const auto old=planar->GetBodyHandle(); const auto parts=planar->GetShapeHandles();
    {
        const Scene::PhysicsMutationDeferral deferred(scene); const auto read=world->Read(); const auto read2D=flat->Read();
        ASSERT_TRUE(scene.DestroyGameObject(object)); EXPECT_EQ(destroys,1);
        Physics::QueryHit hit; EXPECT_TRUE(read->Raycast({0,3,0},{0,-1,0},8,hit)); EXPECT_EQ(read2D->OverlapCircle({0,0},10).size(),parts.size());
    }
    scene.SyncTransforms(); EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_FALSE(flat->IsValid(old)); for(auto shape:parts) EXPECT_FALSE(flat->IsValid(shape));
    EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u); EXPECT_EQ(destroys,1);
}

TEST(PhysicsLifecycleTests, DetachedComponentsAfterStopKeepTheirSceneReleaseContextForBothBodiesAndStaticGroups)
{
    Scene scene; AddBothDimensions(scene,"Bodies");
    auto& spatial=scene.CreateGameObject("Static 3D"); auto* static3D=spatial.AddComponent<Components::PhysicsColliderComponent>();
    auto& planar=scene.CreateGameObject("Static 2D"); auto* static2D=planar.AddComponent<Components::PhysicsCollider2DComponent>();
    static2D->SetShape(Components::PhysicsCollider2DComponent::EColliderShape::POLYGON); static2D->SetVertices({{0,0},{4,0},{4,1},{1,1},{1,4},{0,4}});
    scene.Play(); scene.SyncTransforms(); scene.Stop(); scene.SyncTransforms();
    auto* object3D=scene.FindGameObjectByName("Bodies"); auto* object2D=scene.FindGameObjectByName("Bodies 2D"); ASSERT_TRUE(object3D); ASSERT_TRUE(object2D);
    auto* body3D=object3D->GetComponent<Components::RigidbodyComponent>(); auto* body2D=object2D->GetComponent<Components::Rigidbody2DComponent>();
    auto world=scene.GetPhysicsWorld(); auto flat=scene.GetPhysicsWorld2D();
    const auto a=body3D->GetBodyHandle(),b=static3D->GetBodyHandle(); const auto c=body2D->GetBodyHandle(),d=static2D->GetBodyHandle();
    {
        const Scene::PhysicsMutationDeferral deferred(scene); const auto read=world->Read(); const auto read2D=flat->Read();
        ASSERT_TRUE(object3D->RemoveComponent(body3D)); ASSERT_TRUE(object2D->RemoveComponent(body2D));
        ASSERT_TRUE(spatial.RemoveComponent(static3D)); ASSERT_TRUE(planar.RemoveComponent(static2D));
        EXPECT_EQ(read->OverlapSphere(20,{0,0,0}).size(),2u); EXPECT_GE(read2D->OverlapCircle({0,0},20).size(),3u);
    }
    scene.SyncTransforms(); EXPECT_FALSE(world->IsValid(a)); EXPECT_FALSE(world->IsValid(b)); EXPECT_FALSE(flat->IsValid(c)); EXPECT_FALSE(flat->IsValid(d));
    EXPECT_EQ(world->GetStatistics().bodyCount,1u); EXPECT_EQ(flat->GetStatistics().bodyCount,1u); EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u);
}

TEST(PhysicsLifecycleTests, RetainedWorldsAreEmptyAfterDestroyingAStoppedSceneWithRecreatedQueryGeometry)
{
    auto scene=std::make_unique<Scene>(); AddBothDimensions(*scene,"Bodies"); scene->Play(); scene->SyncTransforms(); scene->Stop(); scene->SyncTransforms();
    auto world=scene->GetPhysicsWorld(); auto flat=scene->GetPhysicsWorld2D(); ASSERT_EQ(world->GetStatistics().bodyCount,1u); ASSERT_EQ(flat->GetStatistics().bodyCount,1u);
    scene.reset(); EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(world->GetStatistics().shapeCount,0u);
    EXPECT_EQ(flat->GetStatistics().bodyCount,0u); EXPECT_EQ(flat->GetStatistics().shapeCount,0u);
}

TEST(PhysicsLifecycleTests, NormalOwnerDestructionKeepsExistingGameplayCallbackAndPhysicsCleanupOrder)
{
    for(bool planar:{false,true})
    {
        Scene scene; auto& object=scene.CreateGameObject("Callback order"); auto* observer=object.AddComponent<DestroyObserver>(); bool observed=false;
        if(planar)
        {
            auto* body=object.AddComponent<Components::Rigidbody2DComponent>(); body->SetGravityScale(0); body->SetVelocity({2,0}); object.AddComponent<Components::PhysicsCollider2DComponent>();
            observer->callback=[&observed,body]{observed=true; EXPECT_TRUE(body->GetBodyHandle().IsValid()); EXPECT_NEAR(body->GetVelocity().x,2,1e-5f);};
        }
        else
        {
            auto* body=object.AddComponent<Components::RigidbodyComponent>(); body->SetUseGravity(false); body->SetVelocity({2,0,0}); object.AddComponent<Components::PhysicsColliderComponent>();
            observer->callback=[&observed,body]{observed=true; EXPECT_TRUE(body->GetBodyHandle().IsValid()); EXPECT_NEAR(body->GetVelocity().x,2,1e-5f);};
        }
        scene.Play(); scene.SyncTransforms(); ASSERT_TRUE(scene.DestroyGameObject(object)); EXPECT_TRUE(observed); scene.SyncTransforms();
        if(planar) EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().bodyCount,0u); else EXPECT_EQ(scene.GetPhysicsWorld()->GetStatistics().bodyCount,0u);
    }
}

Engine::GameObject* ChildNamed(Engine::GameObject& object, const std::string& name)
{
    for (auto* child:object.GetChildren()) if (child && child->GetName()==name) return child;
    return nullptr;
}

struct PhysicsSceneFiles
{
    std::filesystem::path root=std::filesystem::temp_directory_path()/("nullus-physics-lifecycle-"+Guid::New().ToString());
    ~PhysicsSceneFiles() { std::error_code error; std::filesystem::remove_all(root,error); }
};
}

TEST(PhysicsLifecycleTests, DeferredWorldSettingsKeepRetainedQueriesStableUntilSafeCommit)
{
    Scene scene; auto initial=scene.GetPhysicsSettings(); initial.gravity3D={0,0,0}; initial.gravity2D={0,0};
    std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(initial,error));
    AddBothDimensions(scene,"Configured");
    auto* body=scene.FindGameObjectByName("Configured")->GetComponent<Components::RigidbodyComponent>(); body->SetUseGravity(true);
    auto* body2D=scene.FindGameObjectByName("Configured 2D")->GetComponent<Components::Rigidbody2DComponent>(); body2D->SetGravityScale(1);
    scene.Play(); scene.SyncTransforms(); const auto world=scene.GetPhysicsWorld(); const auto planar=scene.GetPhysicsWorld2D();
    const auto original=body->GetBodyHandle(); const auto original2D=body2D->GetBodyHandle();
    const auto revision=world->GetRevision(), revision2D=planar->GetRevision();
    auto settings=initial; settings.gravity3D={2,-3,4}; settings.gravity2D={-4,6}; settings.solverSubsteps2D=8; settings.interpolate=true;
    {
        const Scene::PhysicsMutationDeferral deferral(scene);
        const auto read=world->Read(); const auto read2D=planar->Read();
        ASSERT_TRUE(scene.SetPhysicsSettings(settings,error)) << error;
        EXPECT_TRUE(scene.GetPhysicsSettings()==settings); EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),3u);
        EXPECT_EQ(world->GetRevision(),revision); EXPECT_EQ(planar->GetRevision(),revision2D); EXPECT_EQ(planar->GetSolverSubsteps(),4u);
        EXPECT_FALSE(scene.TrySyncTransforms()); scene.FixedUpdate(0.1f); EXPECT_TRUE(scene.SimulatePhysics(0.1f).empty());
        EXPECT_EQ(world->GetStatistics().stepIndex,0u); EXPECT_EQ(planar->GetStatistics().stepIndex,0u);
        Physics::QueryHit hit; Physics::QueryHit2D hit2D;
        EXPECT_TRUE(read->Raycast({0,3,0},{0,-1,0},10,hit)); EXPECT_TRUE(read2D->Raycast({0,3},{0,-1},10,hit2D));
    }
    ASSERT_TRUE(scene.TrySyncTransforms()); EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u); EXPECT_EQ(planar->GetSolverSubsteps(),8u);
    EXPECT_EQ(body->GetBodyHandle(),original); EXPECT_EQ(body2D->GetBodyHandle(),original2D);
    scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(body->GetVelocity().x,0.2f,1e-5f); EXPECT_NEAR(body->GetVelocity().y,-0.3f,1e-5f);
    EXPECT_NEAR(body2D->GetVelocity().x,-0.4f,1e-5f); EXPECT_NEAR(body2D->GetVelocity().y,0.6f,1e-5f);
}

TEST(PhysicsLifecycleTests, ContactStopWaitsForQueryReleaseAndClearsBothWorldsAtTheNextBoundary)
{
    Scene scene; AddBothDimensions(scene,"Contact");
    scene.FindGameObjectByName("Contact")->GetTransform()->SetWorldPosition({0,0.4f,0});
    auto& floor=scene.CreateGameObject("Floor"); floor.GetTransform()->SetWorldPosition({0,-0.5f,0});
    floor.AddComponent<Components::PhysicsColliderComponent>()->SetSize({5,0.5f,5});
    scene.Play(); scene.SyncTransforms(); const auto world=scene.GetPhysicsWorld(); const auto planar=scene.GetPhysicsWorld2D(); bool delivered=false;
    scene.PhysicsContactEvent.AddListener([&](const Physics::ContactEvent& event)
    {
        if (delivered || event.phase!=Physics::ContactPhase::Enter) return;
        delivered=true; const auto read=world->Read(); const auto read2D=planar->Read();
        const auto revision=world->GetRevision(), revision2D=planar->GetRevision();
        scene.Stop(); scene.Stop(); EXPECT_TRUE(scene.IsPlaying()); EXPECT_TRUE(scene.IsPhysicsStopPending());
        EXPECT_FALSE(scene.TrySyncTransforms()); EXPECT_EQ(world->GetRevision(),revision); EXPECT_EQ(planar->GetRevision(),revision2D);
        Physics::QueryHit hit; Physics::QueryHit2D hit2D;
        EXPECT_TRUE(read->Raycast({0,3,0},{0,-1,0},10,hit)); EXPECT_TRUE(read2D->Raycast({0,3},{0,-1},10,hit2D));
    });
    scene.FixedUpdate(1.0f/60); scene.SimulatePhysics(1.0f/60); ASSERT_TRUE(delivered); EXPECT_TRUE(scene.IsPhysicsStopPending());
    scene.FixedUpdate(1.0f/60); EXPECT_FALSE(scene.IsPlaying()); EXPECT_FALSE(scene.IsPhysicsStopPending());
    EXPECT_EQ(world->GetStatistics().stepIndex,1u); EXPECT_EQ(planar->GetStatistics().stepIndex,1u);
    EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(planar->GetStatistics().bodyCount,0u);
    EXPECT_EQ(scene.GetPendingPhysicsCommandCount(),0u);
}

TEST(PhysicsLifecycleTests, RuntimePrefabInstancesOwnFreshBodiesAndNeverCopyPendingForces)
{
    Scene authoring;
    auto& root=authoring.CreateGameObject("Prefab"); auto* source=root.AddComponent<Components::RigidbodyComponent>(); source->SetUseGravity(false);
    root.AddComponent<Components::PhysicsColliderComponent>();
    auto& child=authoring.CreateGameObject("Child"); child.SetParent(root); child.GetTransform()->SetLocalPosition({2,0,0});
    child.AddComponent<Components::PhysicsColliderComponent>();
    auto& flat=authoring.CreateGameObject("Planar"); flat.SetParent(root); flat.GetTransform()->SetLocalPosition({10,0,0});
    flat.AddComponent<Components::Rigidbody2DComponent>()->SetGravityScale(0); flat.AddComponent<Components::PhysicsCollider2DComponent>();
    auto& flatChild=authoring.CreateGameObject("Planar child"); flatChild.SetParent(flat); flatChild.GetTransform()->SetLocalPosition({2,0,0});
    flatChild.AddComponent<Components::PhysicsCollider2DComponent>();
    authoring.Play(); authoring.SyncTransforms(); source->AddImpulse({99,0,0});
    const auto prefab=Serialize::ObjectGraphSerializer::SerializePrefab(root);
    Scene scene; scene.Play(); const auto world=scene.GetPhysicsWorld(); const auto planar=scene.GetPhysicsWorld2D();
    auto first=Serialize::ObjectGraphInstantiator::InstantiatePrefab(prefab,scene);
    auto second=Serialize::ObjectGraphInstantiator::InstantiatePrefab(prefab,scene);
    ASSERT_NE(first.root,nullptr); ASSERT_NE(second.root,nullptr); second.root->GetTransform()->SetWorldPosition({20,0,0});
    auto* firstBody=first.root->GetComponent<Components::RigidbodyComponent>(); auto* secondBody=second.root->GetComponent<Components::RigidbodyComponent>();
    ASSERT_NE(firstBody,nullptr); ASSERT_NE(secondBody,nullptr); EXPECT_FALSE(firstBody->GetBodyHandle().IsValid()); EXPECT_FALSE(secondBody->GetBodyHandle().IsValid());
    scene.SyncTransforms(); EXPECT_EQ(world->GetStatistics().bodyCount,2u); EXPECT_EQ(world->GetStatistics().shapeCount,4u);
    EXPECT_EQ(planar->GetStatistics().bodyCount,2u); EXPECT_EQ(planar->GetStatistics().shapeCount,4u);
    EXPECT_NE(firstBody->GetBodyHandle(),secondBody->GetBodyHandle()); EXPECT_NE(firstBody->GetBodyHandle().world,source->GetBodyHandle().world);
    auto* firstFlat=ChildNamed(*first.root,"Planar"); auto* secondFlat=ChildNamed(*second.root,"Planar"); ASSERT_NE(firstFlat,nullptr); ASSERT_NE(secondFlat,nullptr);
    auto* first2D=firstFlat->GetComponent<Components::Rigidbody2DComponent>(); auto* second2D=secondFlat->GetComponent<Components::Rigidbody2DComponent>();
    firstBody->AddImpulse({2,0,0}); first2D->AddImpulse({2,0}); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(firstBody->GetVelocity().x,2,1e-5f); EXPECT_GT(first2D->GetVelocity().x,0);
    EXPECT_NEAR(secondBody->GetVelocity().x,0,1e-5f); EXPECT_NEAR(second2D->GetVelocity().x,0,1e-5f); EXPECT_NEAR(source->GetVelocity().x,0,1e-5f);
    const auto removed=firstBody->GetBodyHandle(); const auto removed2D=first2D->GetBodyHandle(); Engine::GameObject* third=nullptr;
    {
        const Scene::PhysicsMutationDeferral deferral(scene); const auto read=world->Read(); const auto read2D=planar->Read();
        ASSERT_TRUE(scene.DestroyGameObject(*first.root)); third=Serialize::ObjectGraphInstantiator::InstantiatePrefab(prefab,scene).root;
        ASSERT_NE(third,nullptr); third->GetTransform()->SetWorldPosition({40,0,0}); EXPECT_FALSE(third->GetComponent<Components::RigidbodyComponent>()->GetBodyHandle().IsValid());
        EXPECT_FALSE(scene.TrySyncTransforms()); EXPECT_TRUE(world->IsValid(removed)); EXPECT_TRUE(planar->IsValid(removed2D));
    }
    scene.SyncTransforms(); EXPECT_FALSE(world->IsValid(removed)); EXPECT_FALSE(planar->IsValid(removed2D));
    EXPECT_EQ(world->GetStatistics().bodyCount,2u); EXPECT_EQ(world->GetStatistics().shapeCount,4u);
    EXPECT_EQ(planar->GetStatistics().bodyCount,2u); EXPECT_EQ(planar->GetStatistics().shapeCount,4u);
    EXPECT_EQ(authoring.GetPhysicsWorld()->GetStatistics().bodyCount,1u); EXPECT_EQ(authoring.GetPhysicsWorld2D()->GetStatistics().bodyCount,1u);
}

TEST(PhysicsLifecycleTests, ReparentingCollidersReassignsShapesAndPreservesIndependentNestedBodies)
{
    for (bool twoDimensional:{false,true})
    {
        Scene scene; auto& a=scene.CreateGameObject("A"); auto& b=scene.CreateGameObject("B"); b.GetTransform()->SetWorldPosition({10,0,0});
        auto& child=scene.CreateGameObject("Shape"); child.SetParent(a); child.GetTransform()->SetLocalPosition({2,0,0});
        auto& nested=scene.CreateGameObject("Nested"); nested.SetParent(child); nested.GetTransform()->SetLocalPosition({2,0,0});
        const auto exercise=[&](auto* first,auto* second,auto* nestedBody,auto* collider,auto* nestedCollider,auto world)
        {
            if constexpr (std::is_same_v<std::remove_pointer_t<decltype(first)>,Components::RigidbodyComponent>)
            { first->SetUseGravity(false); second->SetUseGravity(false); nestedBody->SetUseGravity(false); }
            else { first->SetGravityScale(0); second->SetGravityScale(0); nestedBody->SetGravityScale(0); }
            scene.Play(); scene.SyncTransforms(); const auto original=collider->GetShapeHandle(); const auto independent=nestedBody->GetBodyHandle();
            const auto independentShape=nestedCollider->GetShapeHandle(); EXPECT_EQ(collider->GetBodyHandle(),first->GetBodyHandle());
            {
                const Scene::PhysicsMutationDeferral deferral(scene); const auto read=world->Read();
                child.SetParent(b); EXPECT_FALSE(scene.TrySyncTransforms()); EXPECT_TRUE(world->IsValid(original));
            }
            scene.SyncTransforms(); EXPECT_EQ(collider->GetBodyHandle(),second->GetBodyHandle()); EXPECT_FALSE(world->IsValid(original));
            EXPECT_EQ(nestedBody->GetBodyHandle(),independent); EXPECT_EQ(nestedCollider->GetShapeHandle(),independentShape);
            EXPECT_NEAR(nested.GetTransform()->GetWorldPosition().x,4,1e-5f); EXPECT_NEAR(child.GetTransform()->GetWorldPosition().x,12,1e-5f);
            child.DetachFromParent(); scene.SyncTransforms(); const auto standalone=collider->GetBodyHandle();
            EXPECT_NE(standalone,first->GetBodyHandle()); EXPECT_NE(standalone,second->GetBodyHandle());
            EXPECT_EQ(world->GetStatistics().bodyCount,4u); EXPECT_EQ(world->GetStatistics().shapeCount,2u);
            child.SetParent(a); scene.SyncTransforms(); EXPECT_EQ(collider->GetBodyHandle(),first->GetBodyHandle()); EXPECT_FALSE(world->IsValid(standalone));
            EXPECT_EQ(world->GetStatistics().bodyCount,3u); EXPECT_EQ(world->GetStatistics().shapeCount,2u);
            EXPECT_EQ(nestedBody->GetBodyHandle(),independent); EXPECT_NEAR(nested.GetTransform()->GetWorldPosition().x,4,1e-5f);
        };
        if (twoDimensional)
        {
            auto* first=a.AddComponent<Components::Rigidbody2DComponent>(); auto* second=b.AddComponent<Components::Rigidbody2DComponent>();
            auto* nestedBody=nested.AddComponent<Components::Rigidbody2DComponent>();
            exercise(first,second,nestedBody,child.AddComponent<Components::PhysicsCollider2DComponent>(),nested.AddComponent<Components::PhysicsCollider2DComponent>(),scene.GetPhysicsWorld2D());
        }
        else
        {
            auto* first=a.AddComponent<Components::RigidbodyComponent>(); auto* second=b.AddComponent<Components::RigidbodyComponent>();
            auto* nestedBody=nested.AddComponent<Components::RigidbodyComponent>();
            exercise(first,second,nestedBody,child.AddComponent<Components::PhysicsColliderComponent>(),nested.AddComponent<Components::PhysicsColliderComponent>(),scene.GetPhysicsWorld());
        }
    }
}

TEST(PhysicsLifecycleTests, RepeatedSandboxReplacementRejectsUnsafeDestructionAndPreservesTheEditWorlds)
{
    SceneManager scenes(""); auto* edit=scenes.GetCurrentScene(); AddBothDimensions(*edit,"Durable"); edit->SyncTransforms();
    const auto editWorld=edit->GetPhysicsWorld(); const auto edit2D=edit->GetPhysicsWorld2D();
    PhysicsSceneFiles files; const auto path=(files.root/"durable.scene").string(); ASSERT_TRUE(scenes.SaveSceneToPath(*edit,path)); std::string error;
    ASSERT_TRUE(scenes.BeginPlaySandbox(&error)) << error; scenes.GetCurrentScene()->Play();
    for (int cycle=0;cycle<8;++cycle)
    {
        auto* previous=scenes.GetCurrentScene(); previous->SyncTransforms(); const auto world=previous->GetPhysicsWorld(); const auto planar=previous->GetPhysicsWorld2D();
        const auto previousId=previous->GetInstanceID();
        {
            const Scene::PhysicsMutationDeferral deferral(*previous); const auto read=world->Read(); const auto read2D=planar->Read();
            EXPECT_FALSE(scenes.LoadScene(path,true));
            EXPECT_FALSE(scenes.DiscardPlaySandbox(&error)); scenes.UnloadCurrentScene(); scenes.LoadEmptyScene();
            EXPECT_EQ(scenes.GetCurrentScene(),previous); EXPECT_EQ(world->GetStatistics().bodyCount,1u); EXPECT_EQ(planar->GetStatistics().bodyCount,1u);
        }
        ASSERT_TRUE(scenes.LoadScene(path,true));
        EXPECT_NE(scenes.GetCurrentScene()->GetInstanceID(),previousId); EXPECT_TRUE(scenes.GetCurrentScene()->IsPlaying());
        EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(planar->GetStatistics().bodyCount,0u);
        EXPECT_EQ(editWorld->GetStatistics().bodyCount,1u); EXPECT_EQ(edit2D->GetStatistics().bodyCount,1u);
    }
    ASSERT_TRUE(scenes.DiscardPlaySandbox(&error)); EXPECT_EQ(scenes.GetCurrentScene(),edit); EXPECT_FALSE(edit->IsPlaying());
    EXPECT_EQ(editWorld->GetStatistics().bodyCount,1u); EXPECT_EQ(edit2D->GetStatistics().bodyCount,1u);
}

TEST(PhysicsLifecycleTests, DelayedSceneLoadsWaitForReadReleaseAndPreserveRequestsQueuedByLoadEvents)
{
    PhysicsSceneFiles files; SceneManager scenes(""); Scene first, second;
    AddBothDimensions(first,"First"); AddBothDimensions(second,"Second");
    const auto firstPath=(files.root/"first.scene").string(), secondPath=(files.root/"second.scene").string();
    ASSERT_TRUE(scenes.SaveSceneToPath(first,firstPath)); ASSERT_TRUE(scenes.SaveSceneToPath(second,secondPath));
    auto* edit=scenes.GetCurrentScene(); AddBothDimensions(*edit,"Edit"); std::string error;
    ASSERT_TRUE(scenes.BeginPlaySandbox(&error)); auto* previous=scenes.GetCurrentScene(); previous->Play(); previous->SyncTransforms();
    const auto oldWorld=previous->GetPhysicsWorld(), oldEditWorld=edit->GetPhysicsWorld(); const auto old2D=previous->GetPhysicsWorld2D(); bool queuedSecond=false;
    scenes.SceneLoadEvent.AddListener([&]
    {
        if (!queuedSecond && scenes.GetCurrentScene()->FindGameObjectByName("First"))
        { queuedSecond=true; scenes.LoadAndPlayDelayed(secondPath,true); }
    });
    {
        const Scene::PhysicsMutationDeferral deferral(*previous); const auto read=oldWorld->Read(); const auto read2D=old2D->Read();
        scenes.LoadAndPlayDelayed(firstPath,true); scenes.Update(); EXPECT_EQ(scenes.GetCurrentScene(),previous); EXPECT_FALSE(queuedSecond);
    }
    scenes.Update(); ASSERT_NE(scenes.GetCurrentScene()->FindGameObjectByName("First"),nullptr); EXPECT_TRUE(queuedSecond);
    EXPECT_EQ(oldWorld->GetStatistics().bodyCount,0u); EXPECT_EQ(old2D->GetStatistics().bodyCount,0u);
    scenes.GetCurrentScene()->SyncTransforms(); const auto firstWorld=scenes.GetCurrentScene()->GetPhysicsWorld(); const auto first2D=scenes.GetCurrentScene()->GetPhysicsWorld2D();
    scenes.Update(); ASSERT_NE(scenes.GetCurrentScene()->FindGameObjectByName("Second"),nullptr);
    EXPECT_EQ(firstWorld->GetStatistics().bodyCount,0u); EXPECT_EQ(first2D->GetStatistics().bodyCount,0u);
    ASSERT_TRUE(scenes.DiscardPlaySandbox(&error)); EXPECT_EQ(scenes.GetCurrentScene(),edit); EXPECT_NE(edit->FindGameObjectByName("Edit"),nullptr);
    EXPECT_EQ(edit->GetPhysicsWorld(),oldEditWorld);
}
