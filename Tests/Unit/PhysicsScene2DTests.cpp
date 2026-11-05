#include <gtest/gtest.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Components/Rigidbody2DComponent.h>
#include <Engine/Components/RigidbodyComponent.h>
#include <Engine/Components/PhysicsCollider2DComponent.h>
#include <Engine/Components/PhysicsColliderComponent.h>
#include <Engine/Serialize/ObjectGraphSerializer.h>
#include <Engine/Serialize/ObjectGraphInstantiator.h>
#include <Engine/Serialize/ObjectGraphWriter.h>
#include <Engine/Serialize/ObjectGraphReader.h>
#include <cmath>

namespace
{
using namespace NLS;
using Engine::SceneSystem::Scene;
using Engine::Components::Rigidbody2DComponent;
using Engine::Components::PhysicsCollider2DComponent;
constexpr float Step2D = 1.0f / 60;
void Advance2D(Scene& scene, int count = 1)
{
    for (int i = 0; i < count; ++i) { scene.FixedUpdate(Step2D); scene.SimulatePhysics(Step2D); }
}
}

TEST(PhysicsScene2DTests, EditingPublishesStaticQueriesWithoutRunningSimulation)
{
    Scene scene;
    auto& object = scene.CreateGameObject("Static 2D"); object.GetTransform()->SetWorldPosition({1,2,3});
    auto* collider = object.AddComponent<PhysicsCollider2DComponent>(); collider->SetSize({2,4});
    scene.SyncTransforms(); const auto world = scene.PeekPhysicsWorld2D(); ASSERT_TRUE(world);
    Physics::QueryHit2D hit; ASSERT_TRUE(world->Read()->Raycast({1,10},{0,-1},20,hit));
    EXPECT_NEAR(hit.position.y, 4, 0.001f); EXPECT_EQ(hit.owner, static_cast<uint64_t>(collider->GetInstanceID()));
    object.GetTransform()->SetWorldPosition({3,4,7}); scene.SyncTransforms();
    ASSERT_TRUE(world->Read()->Raycast({3,10},{0,-1},20,hit)); EXPECT_NEAR(hit.position.y, 6, 0.001f);
    scene.SimulatePhysics(Step2D); EXPECT_EQ(world->GetStatistics().stepIndex, 0u);
}

TEST(PhysicsScene2DTests, InterpolationPreservesLiveZAndClearsOnTeleportRestoreAndRebuild)
{
    Scene scene; auto settings=scene.GetPhysicsSettings(); settings.interpolate=true;
    std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error));
    auto& object=scene.CreateGameObject("Interpolated 2D"); object.GetTransform()->SetWorldPosition({0,0,7});
    auto* body=object.AddComponent<Rigidbody2DComponent>(); body->SetGravityScale(0); body->SetVelocity({10,0});
    auto* collider=object.AddComponent<PhysicsCollider2DComponent>();
    scene.Play(); scene.SimulatePhysics(0.1f); scene.SetPhysicsInterpolationAlpha(0.5);
    auto presented=scene.CapturePhysicsPresentationTransform(object);
    EXPECT_TRUE(presented.usesPhysicsPose); EXPECT_NEAR(presented.worldPosition.x,0.5f,1e-5f); EXPECT_FLOAT_EQ(presented.worldPosition.z,7);
    EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x,1,1e-5f);
    // A native restore is observable even before the next scene synchronization.
    const auto snapshot=scene.GetPhysicsWorld2D()->CaptureState(); scene.SimulatePhysics(0.1f);
    ASSERT_TRUE(scene.GetPhysicsWorld2D()->RestoreState(snapshot,error));
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(object).worldPosition.x,1,1e-5f);
    scene.SyncTransforms(); collider->SetSize({2,2}); scene.SyncTransforms();
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(object).worldPosition.x,1,1e-5f);
    body->Teleport({20,3},0,false); body->ApplyPendingCommands();
    presented=scene.CapturePhysicsPresentationTransform(object);
    EXPECT_NEAR(presented.worldPosition.x,20,1e-5f); EXPECT_FLOAT_EQ(presented.worldPosition.z,7);
    object.GetTransform()->SetWorldPosition({20,3,40}); scene.SyncTransforms();
    EXPECT_FLOAT_EQ(scene.CapturePhysicsPresentationTransform(object).worldPosition.z,40);
    EXPECT_EQ(body->GetVelocity(),Maths::Vector2{});
}

TEST(PhysicsScene2DTests, CrossingPiDoesNotRebuildShapesOrResetShortestRotationInterpolation)
{
    constexpr float pi=3.14159265358979323846f;
    Scene scene; auto settings=scene.GetPhysicsSettings(); settings.interpolate=true;
    std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error));
    auto& object=scene.CreateGameObject("Rotating");
    object.GetTransform()->SetWorldRotation({0,0,std::sin((pi-0.05f)*0.5f),std::cos((pi-0.05f)*0.5f)});
    auto* body=object.AddComponent<Rigidbody2DComponent>(); body->SetGravityScale(0); body->SetAngularVelocity(1);
    auto* collider=object.AddComponent<PhysicsCollider2DComponent>();
    scene.Play(); scene.SyncTransforms(); const auto shape=collider->GetShapeHandle(); ASSERT_TRUE(shape.IsValid());
    Physics::BodyState2D initial; ASSERT_TRUE(scene.GetPhysicsWorld2D()->GetState(body->GetBodyHandle(),initial));
    scene.SimulatePhysics(0.1f); scene.SyncTransforms(); scene.SetPhysicsInterpolationAlpha(0.5);
    EXPECT_EQ(collider->GetShapeHandle(),shape);
    const auto rotation=scene.CapturePhysicsPresentationTransform(object).worldRotation;
    Physics::BodyState2D state; ASSERT_TRUE(scene.GetPhysicsWorld2D()->GetState(body->GetBodyHandle(),state));
    const float halfway=initial.rotation+0.5f*std::remainder(state.rotation-initial.rotation,2*pi);
    const float alignment=rotation.z*std::sin(halfway*0.5f)+rotation.w*std::cos(halfway*0.5f);
    EXPECT_NEAR(std::abs(alignment),1,1e-6f);
    EXPECT_GT(std::abs(rotation.z),0.99f); EXPECT_LT(state.rotation,0);
    const auto revision=state.poseRevision; scene.SyncTransforms(); ASSERT_TRUE(scene.GetPhysicsWorld2D()->GetState(body->GetBodyHandle(),state));
    EXPECT_EQ(state.poseRevision,revision);
}

TEST(PhysicsScene2DTests, DynamicPoseIgnoresParentMovementWithoutRunningSimulation)
{
    Scene scene; auto& parent=scene.CreateGameObject("Parent"); auto& child=scene.CreateGameObject("Dynamic"); child.SetParent(parent);
    child.GetTransform()->SetLocalPosition({1,2,3});
    auto* body=child.AddComponent<Rigidbody2DComponent>(); body->SetGravityScale(0); child.AddComponent<PhysicsCollider2DComponent>();
    scene.Play(); scene.SyncTransforms(); parent.GetTransform()->SetWorldPosition({10,0,0}); scene.UpdateGameplay(0);
    EXPECT_EQ(child.GetTransform()->GetWorldPosition(),Maths::Vector3(1,2,3));
    Physics::BodyState2D state; ASSERT_TRUE(scene.GetPhysicsWorld2D()->GetState(body->GetBodyHandle(),state)); EXPECT_EQ(state.position,Maths::Vector2(1,2));
}

TEST(PhysicsScene2DTests, MixedDimensionParentsWriteWorldPosesBeforeTheirDynamicChildren)
{
    Scene scene; auto settings=scene.GetPhysicsSettings(); settings.interpolate=true;
    std::string error; ASSERT_TRUE(scene.SetPhysicsSettings(settings,error));
    auto& child=scene.CreateGameObject("3D child first"); auto& parent=scene.CreateGameObject("2D parent second"); child.SetParent(parent);
    child.GetTransform()->SetLocalPosition({0,5,0});
    auto* childBody=child.AddComponent<Engine::Components::RigidbodyComponent>(); childBody->SetUseGravity(false); childBody->SetVelocity({0,0,2});
    child.AddComponent<Engine::Components::PhysicsColliderComponent>();
    auto* parentBody=parent.AddComponent<Rigidbody2DComponent>(); parentBody->SetGravityScale(0); parentBody->SetVelocity({10,0});
    parent.AddComponent<PhysicsCollider2DComponent>();
    scene.Play(); scene.SimulatePhysics(0.1f); scene.SetPhysicsInterpolationAlpha(0.5);
    EXPECT_NEAR(parent.GetTransform()->GetWorldPosition().x,1,1e-5f);
    EXPECT_NEAR(child.GetTransform()->GetWorldPosition().x,0,1e-5f);
    EXPECT_NEAR(scene.CapturePhysicsPresentationTransform(child).worldPosition.z,0.1f,1e-5f);
    scene.SyncTransforms(); scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(child.GetTransform()->GetWorldPosition().x,0,1e-5f);
    EXPECT_NEAR(child.GetTransform()->GetWorldPosition().z,0.4f,1e-5f);
}

TEST(PhysicsScene2DTests, RuntimeFallsOntoStaticColliderAndStopReleasesWorldObjects)
{
    Scene scene;
    auto& ground = scene.CreateGameObject("Ground"); ground.GetTransform()->SetWorldPosition({0,-0.5f,30});
    ground.AddComponent<PhysicsCollider2DComponent>()->SetSize({10,1});
    auto& object = scene.CreateGameObject("Body"); object.GetTransform()->SetWorldPosition({0,3,4});
    auto* body = object.AddComponent<Rigidbody2DComponent>(); object.AddComponent<PhysicsCollider2DComponent>();
    scene.Play(); Advance2D(scene, 180);
    EXPECT_NEAR(object.GetTransform()->GetWorldPosition().y, 0.5f, 0.03f);
    EXPECT_FLOAT_EQ(object.GetTransform()->GetWorldPosition().z, 4);
    const auto world = scene.PeekPhysicsWorld2D(); ASSERT_TRUE(world); EXPECT_EQ(world->GetStatistics().bodyCount, 2u);
    const auto old = body->GetBodyHandle(); scene.Stop();
    EXPECT_FALSE(body->GetBodyHandle().IsValid()); EXPECT_FALSE(scene.PeekPhysicsWorld2D());
    EXPECT_EQ(world->GetStatistics().bodyCount, 0u);
    Physics::BodyState2D state; EXPECT_FALSE(world->GetState(old, state));
}

TEST(PhysicsScene2DTests, ChildCollidersShareNearestRigidbodyAndNestedRigidbodyStartsAnotherBody)
{
    Scene scene;
    auto& root = scene.CreateGameObject("Root"); root.GetTransform()->SetWorldPosition({0,3,0});
    auto* body = root.AddComponent<Rigidbody2DComponent>();
    auto& left = scene.CreateGameObject("Left"); left.SetParent(root); left.GetTransform()->SetLocalPosition({-1,0,0});
    auto* a = left.AddComponent<PhysicsCollider2DComponent>();
    auto& right = scene.CreateGameObject("Right"); right.SetParent(root); right.GetTransform()->SetLocalPosition({1,0,0});
    auto* b = right.AddComponent<PhysicsCollider2DComponent>();
    scene.Play(); scene.SyncTransforms();
    ASSERT_TRUE(body->GetBodyHandle().IsValid()); EXPECT_EQ(a->GetBodyHandle(), body->GetBodyHandle());
    EXPECT_EQ(b->GetBodyHandle(), body->GetBodyHandle());
    EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().bodyCount, 1u);
    EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().shapeCount, 2u);
    EXPECT_NEAR(body->GetMass(), 2, 1e-5f);
    auto& nested = scene.CreateGameObject("Nested"); nested.SetParent(root);
    auto* nestedBody = nested.AddComponent<Rigidbody2DComponent>();
    auto* nestedCollider = nested.AddComponent<PhysicsCollider2DComponent>(); scene.SyncTransforms();
    EXPECT_EQ(nestedCollider->GetBodyHandle(), nestedBody->GetBodyHandle());
    EXPECT_NE(nestedBody->GetBodyHandle(), body->GetBodyHandle());
    EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().bodyCount, 2u);
}

TEST(PhysicsScene2DTests, ForceCommandsApplyAfterShapeMassAndAreConsumedExactlyOnce)
{
    Scene scene; auto& object = scene.CreateGameObject("Commands");
    auto* body = object.AddComponent<Rigidbody2DComponent>();
    body->SetAutoMass(false); body->SetMass(2); body->SetGravityScale(0);
    object.AddComponent<PhysicsCollider2DComponent>(); scene.Play(); scene.SyncTransforms();
    body->AddImpulse({2,0}); EXPECT_FLOAT_EQ(body->GetVelocity().x, 0);
    scene.FixedUpdate(Step2D); EXPECT_NEAR(body->GetVelocity().x, 1, 1e-5f);
    body->AddForce({2,0}); scene.SimulatePhysics(Step2D);
    EXPECT_NEAR(body->GetVelocity().x, 1 + Step2D, 1e-5f);
    Advance2D(scene); EXPECT_NEAR(body->GetVelocity().x, 1 + Step2D, 1e-5f);
}

TEST(PhysicsScene2DTests, MovingAParentDoesNotTeleportItsDynamicChildBody)
{
    Scene scene; auto& parent = scene.CreateGameObject("Parent"); auto& child = scene.CreateGameObject("Dynamic child");
    child.SetParent(parent); child.GetTransform()->SetLocalPosition({1,2,0});
    child.AddComponent<Rigidbody2DComponent>()->SetGravityScale(0); child.AddComponent<PhysicsCollider2DComponent>();
    scene.Play(); Advance2D(scene);
    parent.GetTransform()->SetWorldPosition({10,0,0}); Advance2D(scene);
    EXPECT_NEAR(child.GetTransform()->GetWorldPosition().x, 1, 1e-5f);
    EXPECT_NEAR(child.GetTransform()->GetWorldPosition().y, 2, 1e-5f);
    Physics::QueryHit2D hit;
    ASSERT_TRUE(scene.GetPhysicsWorld2D()->Read()->Raycast({1,5},{0,-1},10,hit)); EXPECT_NEAR(hit.position.y, 2.5f, 0.01f);
    Advance2D(scene); EXPECT_NEAR(child.GetTransform()->GetWorldPosition().x, 1, 1e-5f);
}

TEST(PhysicsScene2DTests, AuthoredTransformTeleportsOnceAndExplicitTeleportCanResetVelocity)
{
    Scene scene; auto& object = scene.CreateGameObject("Teleport");
    auto* body = object.AddComponent<Rigidbody2DComponent>(); body->SetGravityScale(0); body->SetVelocity({1,0});
    object.AddComponent<PhysicsCollider2DComponent>(); scene.Play(); Advance2D(scene);
    object.GetTransform()->SetWorldPosition({5,6,3}); Advance2D(scene);
    EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x, 5 + Step2D, 1e-5f);
    EXPECT_NEAR(body->GetVelocity().x, 1, 1e-5f);
    body->Teleport({2,3}, 0, false); Advance2D(scene);
    EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x, 2, 1e-5f);
    EXPECT_NEAR(object.GetTransform()->GetWorldPosition().y, 3, 1e-5f);
    EXPECT_EQ(body->GetVelocity(), Maths::Vector2{});
    EXPECT_FLOAT_EQ(object.GetTransform()->GetWorldPosition().z, 3);
}

TEST(PhysicsScene2DTests, InvalidShearRetainsValidShapeAndOtherCollidersRemainUsable)
{
    Scene scene; auto& object = scene.CreateGameObject("Scaled");
    object.GetTransform()->SetLocalScale({2,3,1});
    auto* collider = object.AddComponent<PhysicsCollider2DComponent>(); scene.SyncTransforms();
    const auto old = collider->GetShapeHandle(); ASSERT_TRUE(old.IsValid());
    collider->SetLocalRotation(45); scene.SyncTransforms();
    EXPECT_FALSE(collider->LastError().empty()); EXPECT_EQ(collider->GetShapeHandle(), old);
    EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().shapeCount, 1u);
    auto& other = scene.CreateGameObject("Other"); other.GetTransform()->SetWorldPosition({5,0,0});
    other.AddComponent<PhysicsCollider2DComponent>(); scene.SyncTransforms();
    Physics::QueryHit2D hit; EXPECT_TRUE(scene.GetPhysicsWorld2D()->Read()->Raycast({5,3},{0,-1},10,hit));
}

TEST(PhysicsScene2DTests, RemovingComponentsAndUnloadingSceneLeaveNoBackendBodies)
{
    auto scene = std::make_unique<Scene>(); auto& object = scene->CreateGameObject("Remove");
    auto* collider = object.AddComponent<PhysicsCollider2DComponent>(); scene->SyncTransforms();
    auto world = scene->GetPhysicsWorld2D(); ASSERT_TRUE(world); EXPECT_EQ(world->GetStatistics().bodyCount, 1u);
    ASSERT_TRUE(object.RemoveComponent(collider)); EXPECT_EQ(world->GetStatistics().bodyCount, 0u);
    object.AddComponent<PhysicsCollider2DComponent>(); scene->SyncTransforms(); EXPECT_EQ(world->GetStatistics().bodyCount, 1u);
    scene.reset(); EXPECT_EQ(world->GetStatistics().bodyCount, 0u); EXPECT_EQ(world->GetStatistics().shapeCount, 0u);
}

TEST(PhysicsScene2DTests, SceneRoundTripRestoresConfigurationAndCreatesIndependentWorldHandles)
{
    Scene scene; auto& object = scene.CreateGameObject("Persistent 2D"); object.GetTransform()->SetWorldPosition({1,2,3});
    auto* body = object.AddComponent<Rigidbody2DComponent>(); body->SetAutoMass(false); body->SetMass(7); body->SetGravityScale(0);
    auto* collider = object.AddComponent<PhysicsCollider2DComponent>(); collider->SetShape(PhysicsCollider2DComponent::EColliderShape::CIRCLE);
    collider->SetRadius(0.75f); collider->SetDensity(2); scene.SyncTransforms();
    const auto original = collider->GetBodyHandle();
    const auto document = Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene); ASSERT_FALSE(document.Validate().HasErrors());
    auto loaded = Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document); ASSERT_NE(loaded, nullptr);
    auto* restoredObject = loaded->FindGameObjectByName("Persistent 2D"); ASSERT_TRUE(restoredObject);
    auto* restoredBody = restoredObject->GetComponent<Rigidbody2DComponent>(); ASSERT_TRUE(restoredBody);
    auto* restoredCollider = restoredObject->GetComponent<PhysicsCollider2DComponent>(); ASSERT_TRUE(restoredCollider);
    EXPECT_FALSE(restoredBody->GetAutoMass()); EXPECT_FLOAT_EQ(restoredBody->GetMass(), 7);
    EXPECT_FLOAT_EQ(restoredCollider->GetRadius(), 0.75f); EXPECT_FLOAT_EQ(restoredCollider->GetDensity(), 2);
    loaded->SyncTransforms(); EXPECT_NE(restoredCollider->GetBodyHandle().world, original.world);
    EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().stepIndex, 0u);
    loaded->Play(); Advance2D(*loaded);
    EXPECT_EQ(loaded->GetPhysicsWorld2D()->GetStatistics().stepIndex, 1u);
}

TEST(PhysicsScene2DTests, TwoDimensionsCannotDriveTheSameObjectTransform)
{
    Scene scene; auto& object = scene.CreateGameObject("Conflicting bodies");
    object.AddComponent<Engine::Components::RigidbodyComponent>();
    auto* body = object.AddComponent<Rigidbody2DComponent>(); auto* collider = object.AddComponent<PhysicsCollider2DComponent>();
    object.AddComponent<Engine::Components::PhysicsColliderComponent>();
    scene.Play(); scene.SyncTransforms();
    EXPECT_FALSE(body->GetBodyHandle().IsValid()); EXPECT_FALSE(collider->GetShapeHandle().IsValid());
    EXPECT_FALSE(body->LastError().empty()); EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().bodyCount, 0u);
}

TEST(PhysicsScene2DTests, DynamicsEditsPreservePreviouslySubmittedForceAndBodyIdentity)
{
    Scene scene; auto& object=scene.CreateGameObject("Body"); auto* body=object.AddComponent<Rigidbody2DComponent>();
    body->SetAutoMass(false); body->SetGravityScale(0); object.AddComponent<PhysicsCollider2DComponent>();
    scene.Play(); scene.SyncTransforms(); const auto handle=body->GetBodyHandle();
    body->AddForce({10,0}); scene.FixedUpdate(0.1f); body->SetMass(2); scene.SimulatePhysics(0.1f);
    EXPECT_EQ(body->GetBodyHandle(),handle); const float committedVelocity=body->GetVelocity().x; EXPECT_GT(committedVelocity,0);
    scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f); EXPECT_NEAR(body->GetVelocity().x,committedVelocity,1e-5f);
}

TEST(PhysicsScene2DTests, ConcavePolygonComponentPublishesTheRealContourAndRetainsAllPartsOnInvalidEdit)
{
    Scene scene; auto& object=scene.CreateGameObject("Concave"); auto* collider=object.AddComponent<PhysicsCollider2DComponent>();
    collider->SetShape(PhysicsCollider2DComponent::EColliderShape::POLYGON);
    collider->SetVertices({{0,0},{4,0},{4,1},{1,1},{1,4},{0,4}}); scene.SyncTransforms();
    ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError(); ASSERT_GE(collider->GetShapeHandles().size(),2u);
    const auto body=collider->GetBodyHandle(); const auto shapes=collider->GetShapeHandles(); Physics::QueryHit2D hit;
    ASSERT_TRUE(scene.GetPhysicsWorld2D()->Read()->Raycast({3,5},{0,-1},10,hit)); EXPECT_NEAR(hit.position.y,1,1e-4f); EXPECT_GT(hit.subShapeId,0u);
    EXPECT_TRUE(scene.GetPhysicsWorld2D()->Read()->OverlapCircle({3,3},0.1f).empty()); scene.SyncTransforms(); EXPECT_EQ(collider->GetShapeHandles(),shapes);
    collider->SetVertices({{0,0},{1,1},{0,1},{1,0}}); object.GetTransform()->SetWorldPosition({20,0,0}); scene.SyncTransforms();
    EXPECT_FALSE(collider->LastError().empty()); EXPECT_EQ(collider->GetShapeHandles(),shapes); EXPECT_EQ(collider->GetBodyHandle(),body);
    ASSERT_TRUE(scene.GetPhysicsWorld2D()->Read()->Raycast({3,5},{0,-1},10,hit)); EXPECT_NEAR(hit.position.y,1,1e-4f);
    collider->SetVertices({{0,0},{2,0},{0,2}}); scene.SyncTransforms(); EXPECT_TRUE(collider->LastError().empty());
    for(auto handle:shapes) EXPECT_FALSE(scene.GetPhysicsWorld2D()->IsValid(handle)); EXPECT_EQ(collider->GetBodyHandle(),body);
}

TEST(PhysicsScene2DTests, PolygonBakesMirroredNonuniformLocalRotationAndKeepsPartsWhileItsParentSimulates)
{
    Scene scene; auto& root=scene.CreateGameObject("Root"); root.GetTransform()->SetWorldPosition({10,0,0});
    root.GetTransform()->SetWorldRotation({0,0,0.70710678f,0.70710678f}); auto* body=root.AddComponent<Rigidbody2DComponent>(); body->SetGravityScale(0);
    auto& child=scene.CreateGameObject("Child"); child.SetParent(root); child.GetTransform()->SetLocalPosition({1,0,0}); child.GetTransform()->SetLocalScale({-2,1,1});
    auto* collider=child.AddComponent<PhysicsCollider2DComponent>(); collider->SetShape(PhysicsCollider2DComponent::EColliderShape::POLYGON);
    collider->SetVertices({{0,0},{2,0},{0,2}}); collider->SetLocalRotation(45); scene.Play(); scene.SyncTransforms();
    ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError(); EXPECT_EQ(collider->GetBodyHandle(),body->GetBodyHandle()); Physics::QueryHit2D hit;
    ASSERT_TRUE(scene.GetPhysicsWorld2D()->Read()->Raycast({6,1},{1,0},8,hit)); EXPECT_NEAR(hit.position.x,10-1.41421356f,0.002f);
    EXPECT_FALSE(scene.GetPhysicsWorld2D()->Read()->OverlapCircle({9,1},0.02f).empty()); EXPECT_TRUE(scene.GetPhysicsWorld2D()->Read()->OverlapCircle({9.8f,3.5f},0.02f).empty());
    const auto shapes=collider->GetShapeHandles(); body->SetAngularVelocity(1); Advance2D(scene,120);
    EXPECT_EQ(collider->GetShapeHandles(),shapes); EXPECT_TRUE(collider->LastError().empty())<<collider->LastError();
}

TEST(PhysicsScene2DTests, MirroredChainPreservesFrontSideSupportsBodiesAndRecoversDeletedMembers)
{
    Scene scene; auto& floor=scene.CreateGameObject("Chain"); floor.GetTransform()->SetLocalScale({-1,1,1});
    auto* chain=floor.AddComponent<PhysicsCollider2DComponent>(); chain->SetShape(PhysicsCollider2DComponent::EColliderShape::CHAIN); chain->SetVertices({{5,0},{0,0},{-5,0}});
    auto& ball=scene.CreateGameObject("Ball"); ball.GetTransform()->SetWorldPosition({1,3,0}); ball.AddComponent<Rigidbody2DComponent>();
    auto* circle=ball.AddComponent<PhysicsCollider2DComponent>(); circle->SetShape(PhysicsCollider2DComponent::EColliderShape::CIRCLE); circle->SetRadius(0.25f);
    scene.Play(); scene.SyncTransforms(); ASSERT_TRUE(chain->IsQueryReady())<<chain->LastError(); const auto old=chain->GetShapeHandles(); ASSERT_EQ(old.size(),2u);
    Physics::QueryHit2D hit; ASSERT_TRUE(scene.GetPhysicsWorld2D()->Read()->CircleCast({2,2},0.1f,{0,-1},5,hit));
    EXPECT_FALSE(scene.GetPhysicsWorld2D()->Read()->CircleCast({2,-2},0.1f,{0,1},5,hit)); Advance2D(scene,180); EXPECT_NEAR(ball.GetTransform()->GetWorldPosition().y,0.25f,0.03f);
    ASSERT_TRUE(scene.GetPhysicsWorld2D()->Remove(old.back())); EXPECT_FALSE(chain->IsQueryReady()); scene.SyncTransforms(); ASSERT_TRUE(chain->IsQueryReady());
    for(auto handle:old) EXPECT_FALSE(scene.GetPhysicsWorld2D()->IsValid(handle)); EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().shapeCount,3u);
    chain->SetEnabled(false); scene.SyncTransforms(); EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().shapeCount,1u);
    chain->SetEnabled(true); scene.SyncTransforms(); EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().shapeCount,3u);
    const auto valid=chain->GetShapeHandles(); chain->SetTrigger(true); scene.SyncTransforms(); EXPECT_EQ(chain->GetShapeHandles(),valid); EXPECT_FALSE(chain->LastError().empty());
}

TEST(PhysicsScene2DTests, ContourArraysAndLoopRoundTripThroughTextCloneAndLegacyDefaults)
{
    Scene scene; auto& object=scene.CreateGameObject("Loop"); auto* collider=object.AddComponent<PhysicsCollider2DComponent>();
    collider->SetShape(PhysicsCollider2DComponent::EColliderShape::CHAIN); collider->SetVertices({{0,0},{2,0},{2,2},{0,2}}); collider->SetLoop(true); scene.SyncTransforms(); ASSERT_TRUE(collider->IsQueryReady());
    const auto text=Engine::Serialize::ObjectGraphWriter::Write(Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene)); EXPECT_EQ(text.find("m_shapes"),std::string::npos);
    auto document=Engine::Serialize::ObjectGraphReader::Read(text); ASSERT_TRUE(document); auto clone=Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(*document); ASSERT_TRUE(clone);
    auto* copy=clone->FindGameObjectByName("Loop")->GetComponent<PhysicsCollider2DComponent>(); ASSERT_TRUE(copy); EXPECT_EQ(copy->GetVertices(),collider->GetVertices()); EXPECT_TRUE(copy->IsLoop());
    clone->SyncTransforms(); ASSERT_TRUE(copy->IsQueryReady())<<copy->LastError(); EXPECT_EQ(copy->GetShapeHandles().size(),4u); EXPECT_NE(copy->GetBodyHandle(),collider->GetBodyHandle());
    for(auto& record:document->objects) for(auto it=record.properties.begin();it!=record.properties.end();)
        if(it->name=="vertices" || it->name=="loop") it=record.properties.erase(it); else ++it;
    auto legacy=Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(*document); ASSERT_TRUE(legacy);
    const auto* defaults=legacy->FindGameObjectByName("Loop")->GetComponent<PhysicsCollider2DComponent>(); ASSERT_TRUE(defaults); EXPECT_FALSE(defaults->IsLoop()); EXPECT_EQ(defaults->GetVertices().size(),4u);
}

TEST(PhysicsScene2DTests, DeferredDestructionAndRepeatedPlayReleaseEveryConcavePart)
{
    auto scene=std::make_unique<Scene>(); auto& object=scene->CreateGameObject("Compound"); object.AddComponent<Rigidbody2DComponent>()->SetGravityScale(0);
    auto* collider=object.AddComponent<PhysicsCollider2DComponent>(); collider->SetShape(PhysicsCollider2DComponent::EColliderShape::POLYGON); collider->SetVertices({{0,0},{4,0},{4,1},{1,1},{1,4},{0,4}});
    scene->Play(); scene->SyncTransforms(); auto world=scene->GetPhysicsWorld2D(); const auto count=collider->GetShapeHandles().size(); ASSERT_GE(count,2u);
    for(int i=0;i<4;++i) { scene->Stop(); EXPECT_EQ(world->GetStatistics().bodyCount,0u); scene->Play(); scene->SyncTransforms(); world=scene->GetPhysicsWorld2D(); EXPECT_EQ(world->GetStatistics().shapeCount,count); }
    const auto shapes=collider->GetShapeHandles(); const auto body=collider->GetBodyHandle();
    {
        const Scene::PhysicsMutationDeferral deferred(*scene); const auto read=world->Read(); ASSERT_TRUE(scene->DestroyGameObject(object));
        EXPECT_EQ(read->OverlapCircle({0,0},10).size(),count); EXPECT_TRUE(world->IsValid(body));
    }
    scene->SyncTransforms(); EXPECT_FALSE(world->IsValid(body)); for(auto handle:shapes) EXPECT_FALSE(world->IsValid(handle)); EXPECT_EQ(world->GetStatistics().shapeCount,0u);
    scene.reset(); EXPECT_EQ(world->GetStatistics().bodyCount,0u);
}

TEST(PhysicsScene2DTests, EdgeAndUnsupportedDynamicChainChangesPreserveThePreviousValidGroup)
{
    Scene scene; auto& object=scene.CreateGameObject("Edge"); auto* collider=object.AddComponent<PhysicsCollider2DComponent>();
    collider->SetShape(PhysicsCollider2DComponent::EColliderShape::EDGE); collider->SetVertices({{-2,0},{2,0}}); scene.SyncTransforms(); ASSERT_TRUE(collider->IsQueryReady())<<collider->LastError();
    const auto old=collider->GetShapeHandles(); collider->SetVertices({{0,0},{0.001f,0}}); scene.SyncTransforms(); EXPECT_EQ(collider->GetShapeHandles(),old); EXPECT_FALSE(collider->LastError().empty());
    collider->SetShape(PhysicsCollider2DComponent::EColliderShape::BOX); object.AddComponent<Rigidbody2DComponent>()->SetGravityScale(0); scene.Play(); scene.SyncTransforms();
    const auto box=collider->GetShapeHandles(); ASSERT_EQ(box.size(),1u); collider->SetShape(PhysicsCollider2DComponent::EColliderShape::CHAIN); collider->SetVertices({{2,0},{0,0},{-2,0}}); scene.SyncTransforms();
    EXPECT_EQ(collider->GetShapeHandles(),box); EXPECT_NE(collider->LastError().find("static"),std::string::npos); EXPECT_EQ(scene.GetPhysicsWorld2D()->GetStatistics().shapeCount,1u);
}
