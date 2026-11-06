#include <algorithm>
#include <gtest/gtest.h>
#include <Components/PhysicsColliderComponent.h>
#include <Components/RigidbodyComponent.h>
#include <Components/TransformComponent.h>
#include <SceneSystem/Scene.h>
#include <Serialize/ObjectGraphInstantiator.h>
#include <Serialize/ObjectGraphSerializer.h>
#include <Serialize/ObjectGraphReader.h>
#include <Serialize/ObjectGraphWriter.h>
#include <array>
#include <cmath>
#include <limits>

namespace
{
using namespace NLS;
using Engine::SceneSystem::Scene;
using Engine::Components::RigidbodyComponent;
using Engine::Components::PhysicsColliderComponent;
namespace Serialize=Engine::Serialize;
}

TEST(PhysicsBodyControlsTests, ConstraintsAffectSimulationAndUnlockWithoutReplacingShapes)
{
    Scene scene; auto& object=scene.CreateGameObject("Locked"); auto* body=object.AddComponent<RigidbodyComponent>();
    body->SetUseGravity(false); body->SetAngularDamping(0); body->SetConstraints(1|32); body->SetVelocity({3,4,5}); body->SetAngularVelocity({1,2,3});
    auto* collider=object.AddComponent<PhysicsColliderComponent>(); scene.Play(); scene.SyncTransforms(); const auto original=body->GetBodyHandle(); const auto shape=collider->GetShapeHandle();
    scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x,0,1e-5f); EXPECT_NEAR(object.GetTransform()->GetWorldPosition().y,0.4f,1e-5f);
    EXPECT_NEAR(body->GetAngularVelocity().z,0,1e-5f);
    body->SetConstraints(0); body->SetVelocity({3,0,0}); body->SetAngularVelocity({}); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_EQ(body->GetBodyHandle(),original); EXPECT_EQ(collider->GetShapeHandle(),shape); EXPECT_NEAR(object.GetTransform()->GetWorldPosition().x,0.3f,1e-5f);
    body->SetConstraints(63); body->SetConstraints(64); body->SetVelocity({10,10,10}); body->SetAngularVelocity({10,10,10});
    const auto position=object.GetTransform()->GetWorldPosition(); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_EQ(body->GetConstraints(),63u); EXPECT_EQ(object.GetTransform()->GetWorldPosition(),position);
}

TEST(PhysicsBodyControlsTests, ManualMassPropertiesUsePhysicalBodyUnitsAndSurviveGeometryRebuild)
{
    Scene scene; auto& object=scene.CreateGameObject("Manual mass"); object.GetTransform()->SetLocalScale({2,2,2});
    auto* body=object.AddComponent<RigidbodyComponent>(); body->SetUseGravity(false); body->SetAngularDamping(0); body->SetMass(2); body->SetAutoMassProperties(false);
    body->SetCenterOfMass({0,1,0}); body->SetInertiaTensor({2,3,4}); auto* collider=object.AddComponent<PhysicsColliderComponent>();
    scene.Play(); scene.SyncTransforms(); const auto original=body->GetBodyHandle(); const auto shape=collider->GetShapeHandle();
    body->AddForceAtPosition({1,0,0},{0,0,0},Physics::ForceMode::Impulse); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_NEAR(body->GetVelocity().x,0.5f,1e-5f); EXPECT_NEAR(body->GetAngularVelocity().z,0.25f,1e-5f);
    collider->SetSize({2,0.5f,0.5f}); scene.SyncTransforms(); Physics::BodyState state;
    ASSERT_TRUE(scene.GetPhysicsWorld()->GetState(original,state)); EXPECT_EQ(body->GetBodyHandle(),original); EXPECT_NE(collider->GetShapeHandle(),shape);
    EXPECT_EQ(state.centerOfMass,Maths::Vector3(0,1,0)); EXPECT_EQ(state.inertia,Maths::Vector3(2,3,4)); EXPECT_FLOAT_EQ(state.mass,2);
    EXPECT_NEAR(state.velocity.x,0.5f,1e-5f); EXPECT_NEAR(state.angularVelocity.z,0.25f,1e-5f);
    body->SetAutoMassProperties(true); scene.SyncTransforms(); ASSERT_TRUE(scene.GetPhysicsWorld()->GetState(original,state));
    EXPECT_EQ(state.centerOfMass,Maths::Vector3{}); EXPECT_GT(std::abs(state.inertia.z-4),0.5f); EXPECT_EQ(body->GetBodyHandle(),original);
}

TEST(PhysicsBodyControlsTests, SleepWakeAndGeometryRebuildRespectDeferredCommandsAndCurrentSleepingState)
{
    Scene scene; auto& object=scene.CreateGameObject("Sleep control"); auto* body=object.AddComponent<RigidbodyComponent>();
    body->SetUseGravity(false); body->SetStartAwake(false); body->SetSleepThreshold(0); auto* collider=object.AddComponent<PhysicsColliderComponent>();
    scene.Play(); scene.SyncTransforms(); ASSERT_TRUE(body->IsSleeping());
    body->WakeUp(); body->AddForce({10,0,0}); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
    EXPECT_FALSE(body->IsSleeping()); EXPECT_NEAR(body->GetVelocity().x,1,1e-5f);
    collider->SetSize({1,1,1}); scene.SyncTransforms(); EXPECT_FALSE(body->IsSleeping()); EXPECT_NEAR(body->GetVelocity().x,1,1e-5f);
    const auto world=scene.GetPhysicsWorld();
    {
        const Scene::PhysicsMutationDeferral deferral(scene); const auto read=world->Read(); const auto revision=world->GetRevision();
        body->Sleep(); body->ApplyPendingCommands(); EXPECT_FALSE(body->IsSleeping()); EXPECT_EQ(world->GetRevision(),revision);
    }
    scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f); EXPECT_TRUE(body->IsSleeping()); EXPECT_EQ(body->GetVelocity(),Maths::Vector3{});
    collider->SetRadius(0.25f); collider->SetShape(PhysicsColliderComponent::EColliderShape::SPHERE); scene.SyncTransforms(); EXPECT_TRUE(body->IsSleeping());
    body->WakeUp(); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f); EXPECT_FALSE(body->IsSleeping());
}

TEST(PhysicsBodyControlsTests, AllForceAndTorqueModesAreAppliedExactlyOnceAndRejectInvalidModes)
{
    const std::array<float,4> linear{0.1f,0.2f,1,2}, angular{0.1f,0.4f,1,4}, positioned{-0.05f,-0.1f,-0.5f,-1};
    for (uint32_t mode=0;mode<4;++mode)
    {
        Scene scene; auto& object=scene.CreateGameObject("Force modes"); auto* body=object.AddComponent<RigidbodyComponent>();
        body->SetUseGravity(false); body->SetAngularDamping(0); body->SetMass(2); body->SetAutoMassProperties(false); body->SetInertiaTensor({4,4,4});
        object.AddComponent<PhysicsColliderComponent>(); scene.Play();
        body->AddForceWithMode({2,0,0},mode); body->AddTorqueWithMode({0,0,4},mode); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
        EXPECT_NEAR(body->GetVelocity().x,linear[mode],1e-5f); EXPECT_NEAR(body->GetAngularVelocity().z,angular[mode],1e-5f);
        scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f); EXPECT_NEAR(body->GetVelocity().x,linear[mode],1e-5f); EXPECT_NEAR(body->GetAngularVelocity().z,angular[mode],1e-5f);
        body->SetVelocity({}); body->SetAngularVelocity({}); const auto center=object.GetTransform()->GetWorldPosition();
        body->AddForceAtPositionWithMode({2,0,0},center+Maths::Vector3{0,1,0},mode); scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f);
        EXPECT_NEAR(body->GetVelocity().x,linear[mode],1e-5f); EXPECT_NEAR(body->GetAngularVelocity().z,positioned[mode],1e-5f);
        body->AddForceWithMode({100,0,0},99); body->AddTorqueWithMode({0,0,100},99); body->AddForceAtPositionWithMode({100,0,0},{0,100,0},99);
        scene.FixedUpdate(0.1f); scene.SimulatePhysics(0.1f); EXPECT_NEAR(body->GetVelocity().x,linear[mode],1e-5f); EXPECT_NEAR(body->GetAngularVelocity().z,positioned[mode],1e-5f);
    }
}

TEST(PhysicsBodyControlsTests, NewConfigurationRoundTripsAndMissingLegacyFieldsUseDefaults)
{
    Scene scene; auto& object=scene.CreateGameObject("Serialized body"); auto* body=object.AddComponent<RigidbodyComponent>(); object.AddComponent<PhysicsColliderComponent>();
    body->SetMass(2); body->SetAutoMassProperties(false); body->SetCenterOfMass({1,2,3}); body->SetInertiaTensor({4,5,6});
    body->SetMassFrameRotation({0,0,std::sqrt(0.5f),std::sqrt(0.5f)}); body->SetConstraints(9); body->SetStartAwake(false); body->SetSleepThreshold(0.01f);
    const auto document=Serialize::ObjectGraphSerializer::SerializeScene(scene);
    const auto decoded=Serialize::ObjectGraphReader::Read(Serialize::ObjectGraphWriter::Write(document)); ASSERT_TRUE(decoded);
    auto clone=Serialize::ObjectGraphInstantiator::InstantiateScene(*decoded); ASSERT_TRUE(clone);
    auto* copied=clone->FindGameObjectByName("Serialized body")->GetComponent<RigidbodyComponent>(); ASSERT_NE(copied,nullptr);
    EXPECT_FALSE(copied->GetAutoMassProperties()); EXPECT_EQ(copied->GetCenterOfMass(),Maths::Vector3(1,2,3)); EXPECT_EQ(copied->GetInertiaTensor(),Maths::Vector3(4,5,6));
    EXPECT_EQ(copied->GetConstraints(),9u); EXPECT_FALSE(copied->GetStartAwake()); EXPECT_FLOAT_EQ(copied->GetSleepThreshold(),0.01f);
    clone->Play(); clone->SyncTransforms(); Physics::BodyState state; ASSERT_TRUE(clone->GetPhysicsWorld()->GetState(copied->GetBodyHandle(),state));
    EXPECT_EQ(state.centerOfMass,Maths::Vector3(1,2,3)); EXPECT_NEAR(state.inertia.x,4,1e-5f); EXPECT_NEAR(state.inertia.y,5,1e-5f); EXPECT_NEAR(state.inertia.z,6,1e-5f); EXPECT_FALSE(state.awake);
    EXPECT_NEAR(state.massFrameRotation.z,std::sqrt(0.5f),1e-5f);
    auto legacy=document; size_t removed=0;
    for (auto& record:legacy.objects) if (record.typeName.find("RigidbodyComponent")!=std::string::npos)
        removed+=std::erase_if(record.properties,[](const auto& property)
        { return property.name=="constraints" || property.name=="autoMassProperties" || property.name=="centerOfMass" || property.name=="inertiaTensor" ||
            property.name=="massFrameRotation" || property.name=="startAwake" || property.name=="sleepThreshold"; });
    ASSERT_EQ(removed,7u); auto old=Serialize::ObjectGraphInstantiator::InstantiateScene(legacy); ASSERT_TRUE(old);
    const auto* restored=old->FindGameObjectByName("Serialized body")->GetComponent<RigidbodyComponent>(); ASSERT_NE(restored,nullptr);
    EXPECT_TRUE(restored->GetAutoMassProperties()); EXPECT_EQ(restored->GetConstraints(),0u); EXPECT_TRUE(restored->GetStartAwake());
    EXPECT_FLOAT_EQ(restored->GetSleepThreshold(),0.005f); EXPECT_EQ(restored->GetCenterOfMass(),Maths::Vector3{}); EXPECT_EQ(restored->GetInertiaTensor(),Maths::Vector3(1,1,1));
}

TEST(PhysicsBodyControlsTests, ColliderQueriesComposeNativeLocalPoseAndKeepCapsuleAxesConsistent)
{
    Scene scene; auto& object=scene.CreateGameObject("Rotated capsule"); object.GetTransform()->SetWorldPosition({2,0,0});
    object.GetTransform()->SetWorldRotation({0,0,std::sqrt(0.5f),std::sqrt(0.5f)});
    object.AddComponent<RigidbodyComponent>()->SetUseGravity(false); auto* collider=object.AddComponent<PhysicsColliderComponent>();
    collider->SetShape(PhysicsColliderComponent::EColliderShape::CAPSULE); collider->SetRadius(0.25f); collider->SetHalfHeight(1); collider->SetCenter({0,1,0});
    auto& obstacle=scene.CreateGameObject("Target"); obstacle.GetTransform()->SetWorldPosition({5,0,0}); auto* target=obstacle.AddComponent<PhysicsColliderComponent>();
    scene.Play(); scene.SyncTransforms(); auto read=scene.GetPhysicsWorld()->Read(); Maths::Vector3 position; Maths::Quaternion rotation;
    ASSERT_TRUE(read->GetShapeWorldPose(collider->GetShapeHandle(),position,rotation)); EXPECT_NEAR(position.x,1,1e-5f); EXPECT_NEAR(position.y,0,1e-5f);
    Physics::QueryGeometry3D geometry; geometry.type=Physics::QueryGeometryType3D::Shape; geometry.shape=collider->GetShapeHandle(); Physics::QueryHit hit;
    ASSERT_TRUE(read->SweepShape(geometry,position,rotation,{1,0,0},10,hit)); EXPECT_EQ(hit.shape,target->GetShapeHandle()); EXPECT_NEAR(hit.distance,2.25f,1e-4f);
}
