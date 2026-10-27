#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "Components/ParticleSystemComponent.h"
#include "Components/PhysicsColliderComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphSerializer.h"
#include <Physics/PhysicsWorld.h>
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

    ASSERT_TRUE(object.RemoveComponent(collider));
    scene.Update(0);
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
