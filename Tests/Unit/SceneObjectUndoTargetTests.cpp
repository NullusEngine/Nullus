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
TEST(SceneObjectUndoTargetTests, SavedSceneObjectsRemainEditableWithoutReplacingTheirDocumentIdentity)
{
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();
    Engine::SceneSystem::Scene scene;
    auto& gameObject = scene.CreateGameObject("Saved Scene Gizmo Target");
    const UndoScopeId scope {UndoScopeKind::Scene, "saved-scene-gizmo-target", 9u};
    const auto identifier = Engine::Serialize::ObjectIdentifier::Asset(
        Engine::Serialize::AssetId(NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")), 10, "Assets/Rect.scene");
    ASSERT_NE(persistentManager.BindObjectIdentifier(gameObject, identifier), InstanceID_None);

    const auto target = SceneObjectUndoTarget::Capture(gameObject, scope);
    ASSERT_TRUE(target.has_value());
    EXPECT_TRUE(target->IsValid());
    EXPECT_EQ(target->GetObjectIdentifier(), identifier);
    EXPECT_EQ(target->Resolve(), &gameObject);
    EXPECT_TRUE(target->Matches(gameObject));
    Engine::Serialize::ObjectIdentifier current;
    ASSERT_TRUE(persistentManager.InstanceIDToObjectIdentifier(gameObject.GetInstanceID(), current));
    EXPECT_EQ(current, identifier);
    ASSERT_TRUE(scene.DestroyGameObject(gameObject));
    EXPECT_EQ(target->Resolve(), nullptr);
    persistentManager.Clear();
}

TEST(SceneObjectUndoTargetTests, DocumentGuidSeparatesAffectedKeysWithTheSameFileId)
{
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();
    Engine::SceneSystem::Scene scene;
    auto& first = scene.CreateGameObject("First saved target");
    auto& second = scene.CreateGameObject("Second saved target");
    const UndoScopeId scope {UndoScopeKind::Scene, "saved-scene-gizmo-keys", 10u};
    const auto firstId = Engine::Serialize::ObjectIdentifier::Asset(
        Engine::Serialize::AssetId(NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")), 10);
    const auto secondId = Engine::Serialize::ObjectIdentifier::Asset(
        Engine::Serialize::AssetId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")), 10);
    ASSERT_NE(persistentManager.BindObjectIdentifier(first, firstId), InstanceID_None);
    ASSERT_NE(persistentManager.BindObjectIdentifier(second, secondId), InstanceID_None);
    const auto firstTarget = SceneObjectUndoTarget::Capture(first, scope);
    const auto secondTarget = SceneObjectUndoTarget::Capture(second, scope);
    ASSERT_TRUE(firstTarget); ASSERT_TRUE(secondTarget);
    EXPECT_NE(firstTarget->GetAffectedKey(), secondTarget->GetAffectedKey());
    EXPECT_EQ(firstTarget->Resolve(), &first);
    EXPECT_EQ(secondTarget->Resolve(), &second);
    persistentManager.Clear();
}
} // namespace NLS::Editor::Undo::Tests
