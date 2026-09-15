#include <gtest/gtest.h>

#include "Components/TransformComponent.h"
#include "SceneSystem/Scene.h"
#include "Serialize/PPtr.h"
#include "Undo/ObjectUndoTarget.h"
#include "Undo/SceneObjectUndoTarget.h"

namespace NLS::Editor::Undo::Tests
{
TEST(ObjectUndoTargetTests, ResolvesSceneAndComponentOnlyAtExecutionTime)
{
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    Engine::SceneSystem::Scene scene;
    auto& gameObject = scene.CreateGameObject("Generic Undo Target");
    const UndoScopeId sceneScope {UndoScopeKind::Scene, "generic-object-target", 3u};

    const auto sceneTarget = ObjectUndoTarget::Capture(scene, sceneScope);
    ASSERT_TRUE(sceneTarget.has_value());
    EXPECT_EQ(sceneTarget->Resolve(), &scene);
    EXPECT_EQ(sceneTarget->ResolveAs<Engine::SceneSystem::Scene>(), &scene);
    EXPECT_EQ(sceneTarget->ResolveAs<Engine::GameObject>(), nullptr);

    auto* transform = gameObject.GetTransform();
    ASSERT_NE(transform, nullptr);
    const auto componentTarget = ObjectUndoTarget::Capture(*transform, sceneScope);
    ASSERT_TRUE(componentTarget.has_value());
    EXPECT_EQ(componentTarget->ResolveAs<Engine::Components::TransformComponent>(), transform);
    EXPECT_EQ(componentTarget->ResolveAs<Engine::Components::Component>(), transform);
    EXPECT_EQ(componentTarget->ResolveAs<Engine::GameObject>(), nullptr);
    EXPECT_EQ(componentTarget->GetAffectedKey(),
        "object:" + std::to_string(componentTarget->GetObjectIdentifier().localIdentifierInFile));

    ASSERT_TRUE(scene.DestroyGameObject(gameObject));
    EXPECT_EQ(componentTarget->Resolve(), nullptr);
    EXPECT_EQ(sceneTarget->Resolve(), &scene);

    persistentManager.Clear();
}

TEST(SceneObjectUndoTargetTests, CapturesPersistentLocalIdentifierWithoutRetainingTheGameObject)
{
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    Engine::SceneSystem::Scene scene;
    auto& gameObject = scene.CreateGameObject("Undo Target");
    const UndoScopeId scope {UndoScopeKind::Scene, "scene-object-target", 7u};

    const auto target = SceneObjectUndoTarget::Capture(gameObject, scope);
    ASSERT_TRUE(target.has_value());
    EXPECT_TRUE(target->IsValid());
    EXPECT_EQ(target->GetScope(), scope);
    EXPECT_TRUE(target->GetObjectIdentifier().IsLocalObject());
    EXPECT_EQ(target->Resolve(), &gameObject);
    EXPECT_TRUE(target->Matches(gameObject));

    InstanceID resolvedInstanceId = InstanceID_None;
    ASSERT_TRUE(persistentManager.TryGetInstanceID(target->GetObjectIdentifier(), resolvedInstanceId));
    EXPECT_EQ(resolvedInstanceId, gameObject.GetInstanceID());
    EXPECT_EQ(target->GetAffectedKey(),
        "scene-object:" + std::to_string(target->GetObjectIdentifier().localIdentifierInFile));

    ASSERT_TRUE(scene.DestroyGameObject(gameObject));
    EXPECT_EQ(target->Resolve(), nullptr);

    // A stale handle remains a lookup of its original identifier only.  It
    // must not reserve or synthesize a replacement InstanceID during Resolve.
    InstanceID staleInstanceId = InstanceID_None;
    ASSERT_TRUE(persistentManager.TryGetInstanceID(target->GetObjectIdentifier(), staleInstanceId));
    EXPECT_EQ(staleInstanceId, resolvedInstanceId);

    persistentManager.Clear();
}
} // namespace NLS::Editor::Undo::Tests
