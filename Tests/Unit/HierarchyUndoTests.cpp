#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>

#include "GameObject.h"
#include "Inspector/EditorSelectionService.h"
#include "Panels/HierarchyUndo.h"
#include "SceneSystem/Scene.h"
#include "Serialize/PPtr.h"
#include "Scripting/ScriptComponent.h"
#include "Scripting/ScriptTypes.h"
#include "Undo/ObjectUndoTarget.h"

namespace NLS::Editor::Panels::HierarchyUndo::Tests
{
namespace
{
Undo::UndoScopeId SceneScope(std::string key)
{
    return {Undo::UndoScopeKind::Scene, std::move(key), 1u};
}

void ExpectChildren(
    const Engine::GameObject& parent,
    const std::initializer_list<Engine::GameObject*> expected)
{
    ASSERT_EQ(parent.GetChildren().size(), expected.size());
    size_t index = 0u;
    for (auto* child : expected)
        EXPECT_EQ(parent.GetChildren()[index++], child);
}
} // namespace

TEST(HierarchyUndoTests, RenameRoundTripsThroughTheGlobalTimeline)
{
    Undo::EditorUndoService undo;
    Engine::GameObject gameObject("Before");

    ASSERT_TRUE(RenameGameObject(undo, gameObject, "After", SceneScope("rename-scene")).Succeeded());
    EXPECT_EQ(gameObject.GetName(), "After");
    EXPECT_EQ(undo.GetUndoLabel(), "Rename GameObject");

    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(gameObject.GetName(), "Before");
    ASSERT_EQ(undo.RequestRedo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(gameObject.GetName(), "After");
}

TEST(HierarchyUndoTests, ConsecutiveRenameCommitsMergeIntoOneHistoryEntry)
{
    Undo::EditorUndoService undo;
    Engine::GameObject gameObject("Before");

    ASSERT_TRUE(RenameGameObject(undo, gameObject, "First", SceneScope("rename-merge-scene")).Succeeded());
    ASSERT_TRUE(RenameGameObject(undo, gameObject, "After", SceneScope("rename-merge-scene")).Succeeded());
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    EXPECT_EQ(gameObject.GetName(), "After");

    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(gameObject.GetName(), "Before");
    ASSERT_EQ(undo.RequestRedo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(gameObject.GetName(), "After");
}

TEST(HierarchyUndoTests, ReparentRestoresTheOriginalSiblingOrder)
{
    Undo::EditorUndoService undo;
    Engine::SceneSystem::Scene scene;
    auto& sourceParent = scene.CreateGameObject("Source Parent");
    auto& destinationParent = scene.CreateGameObject("Destination Parent");
    auto& first = scene.CreateGameObject("First");
    auto& moved = scene.CreateGameObject("Moved");
    auto& last = scene.CreateGameObject("Last");

    first.SetParent(sourceParent);
    moved.SetParent(sourceParent);
    last.SetParent(sourceParent);
    ASSERT_TRUE(ReparentGameObject(undo, moved, &destinationParent, SceneScope("reparent-scene")).Succeeded());
    EXPECT_EQ(moved.GetParent(), &destinationParent);
    ExpectChildren(sourceParent, {&first, &last});
    ExpectChildren(destinationParent, {&moved});

    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(moved.GetParent(), &sourceParent);
    ExpectChildren(sourceParent, {&first, &moved, &last});
    ExpectChildren(destinationParent, {});

    ASSERT_EQ(undo.RequestRedo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(moved.GetParent(), &destinationParent);
    ExpectChildren(sourceParent, {&first, &last});
    ExpectChildren(destinationParent, {&moved});
}

TEST(HierarchyUndoTests, DetachToRootRestoresTheOriginalParentAndSiblingOrder)
{
    Undo::EditorUndoService undo;
    Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("Parent");
    auto& first = scene.CreateGameObject("First");
    auto& moved = scene.CreateGameObject("Moved");
    auto& last = scene.CreateGameObject("Last");

    first.SetParent(parent);
    moved.SetParent(parent);
    last.SetParent(parent);
    ASSERT_TRUE(ReparentGameObject(undo, moved, nullptr, SceneScope("detach-scene")).Succeeded());
    EXPECT_EQ(moved.GetParent(), nullptr);
    ExpectChildren(parent, {&first, &last});

    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(moved.GetParent(), &parent);
    ExpectChildren(parent, {&first, &moved, &last});

    ASSERT_EQ(undo.RequestRedo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(moved.GetParent(), nullptr);
    ExpectChildren(parent, {&first, &last});
}

TEST(HierarchyUndoTests, ExternalSiblingChangesReportConflictWithoutMovingTheTimeline)
{
    Undo::EditorUndoService undo;
    Engine::SceneSystem::Scene scene;
    auto& sourceParent = scene.CreateGameObject("Source Parent");
    auto& destinationParent = scene.CreateGameObject("Destination Parent");
    auto& moved = scene.CreateGameObject("Moved");
    auto& externalChild = scene.CreateGameObject("External Child");

    moved.SetParent(sourceParent);
    ASSERT_TRUE(ReparentGameObject(undo, moved, &destinationParent, SceneScope("conflict-scene")).Succeeded());
    externalChild.SetParent(destinationParent);

    const auto result = undo.RequestUndo();
    EXPECT_EQ(result.state, Undo::UndoRequestState::Conflict);
    EXPECT_EQ(undo.GetCursor(), 1u);
    EXPECT_EQ(moved.GetParent(), &destinationParent);
    ExpectChildren(destinationParent, {&moved, &externalChild});
}

TEST(HierarchyUndoTests, DestroyedTargetStopsAtTheStaleHistoryEntry)
{
    Undo::EditorUndoService undo;
    auto gameObject = std::make_unique<Engine::GameObject>("Before");

    ASSERT_TRUE(RenameGameObject(undo, *gameObject, "After", SceneScope("stale-scene")).Succeeded());
    gameObject.reset();

    const auto stale = undo.RequestUndo();
    EXPECT_EQ(stale.state, Undo::UndoRequestState::Stale);
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    EXPECT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Empty);
}

TEST(HierarchyUndoTests, AddingAScriptComponentRoundTripsWithTheSameComponentId)
{
    Undo::EditorUndoService undo;
    Engine::GameObject gameObject("Script Owner");
    Scripting::ScriptAsset script;
    script.sourcePath = "Assets/Scripts/Rotate.cs";
    script.sourceText = "public class Rotate : MonoBehaviour {}";
    script.scriptType = 42u;
    script.contentHash = 7u;
    script.isComponent = true;

    ASSERT_TRUE(AddScriptComponent(undo, gameObject, script, SceneScope("script-component-scene")).Succeeded());
    ASSERT_EQ(gameObject.GetComponents().size(), 2u);
    auto* firstComponent = dynamic_cast<Scripting::ScriptComponent*>(gameObject.GetComponents().back().get());
    ASSERT_NE(firstComponent, nullptr);
    const auto componentId = firstComponent->GetInstanceID();
    EXPECT_EQ(firstComponent->GetScriptAsset().sourcePath, script.sourcePath);

    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    ASSERT_EQ(gameObject.GetComponents().size(), 1u);
    EXPECT_EQ(Object::IDToPointer(componentId), nullptr);

    ASSERT_EQ(undo.RequestRedo().state, Undo::UndoRequestState::Completed);
    ASSERT_EQ(gameObject.GetComponents().size(), 2u);
    auto* restoredComponent = dynamic_cast<Scripting::ScriptComponent*>(gameObject.GetComponents().back().get());
    ASSERT_NE(restoredComponent, nullptr);
    EXPECT_EQ(restoredComponent->GetInstanceID(), componentId);
    EXPECT_EQ(restoredComponent->GetScriptAsset().contentHash, script.contentHash);
}

TEST(HierarchyUndoTests, DeleteSubtreeRestoresStableIdsParentOrderAndSelection)
{
    Undo::EditorUndoService undo;
    Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("Parent");
    auto& first = scene.CreateGameObject("First");
    auto& deleted = scene.CreateGameObject("Deleted");
    auto& descendant = scene.CreateGameObject("Descendant");
    auto& last = scene.CreateGameObject("Last");
    first.SetParent(parent);
    deleted.SetParent(parent);
    descendant.SetParent(deleted);
    last.SetParent(parent);

    const auto deletedId = deleted.GetInstanceID();
    const auto deletedTransformId = deleted.GetTransform()->GetInstanceID();
    const auto descendantId = descendant.GetInstanceID();
    const auto descendantTransformId = descendant.GetTransform()->GetInstanceID();
    Inspector::EditorSelectionService::Get().Select(
        Inspector::InspectorTargetHandle::FromObject(deleted, Inspector::InspectorTargetKind::GameObject));

    ASSERT_TRUE(DeleteGameObject(undo, deleted, SceneScope("delete-subtree-scene")).Succeeded());
    ExpectChildren(parent, {&first, &last});
    EXPECT_EQ(Object::IDToPointer(deletedId), nullptr);
    EXPECT_EQ(Object::IDToPointer(descendantId), nullptr);
    EXPECT_TRUE(Inspector::EditorSelectionService::Get().GetSelection().Empty());

    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    auto* restored = Object::IDToPointer<Engine::GameObject>(deletedId);
    auto* restoredDescendant = Object::IDToPointer<Engine::GameObject>(descendantId);
    ASSERT_NE(restored, nullptr);
    ASSERT_NE(restoredDescendant, nullptr);
    EXPECT_EQ(restored->GetParent(), &parent);
    ExpectChildren(parent, {&first, restored, &last});
    EXPECT_EQ(restoredDescendant->GetParent(), restored);
    EXPECT_EQ(restored->GetTransform()->GetInstanceID(), deletedTransformId);
    EXPECT_EQ(restoredDescendant->GetTransform()->GetInstanceID(), descendantTransformId);
    const auto* selection = Inspector::EditorSelectionService::Get().GetSelection().Primary();
    ASSERT_NE(selection, nullptr);
    EXPECT_EQ(selection->instanceId, deletedId);

    ASSERT_EQ(undo.RequestRedo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(Object::IDToPointer(deletedId), nullptr);
    ExpectChildren(parent, {&first, &last});
    Inspector::EditorSelectionService::Get().Clear();
}

TEST(HierarchyUndoTests, DeleteRestoreResolvesTheOriginalObjectIdentifier)
{
    Undo::EditorUndoService undo;
    Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("Parent");
    auto& deleted = scene.CreateGameObject("Deleted");
    deleted.SetParent(parent);
    const auto scope = SceneScope("delete-object-identifier-scene");
    const auto target = Undo::ObjectUndoTarget::Capture(deleted, scope);
    ASSERT_TRUE(target.has_value());
    const auto identifier = target->GetObjectIdentifier();

    ASSERT_TRUE(DeleteGameObject(undo, deleted, scope).Succeeded());
    EXPECT_EQ(target->Resolve(), nullptr);

    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    auto* restored = target->ResolveAs<Engine::GameObject>();
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->GetParent(), &parent);

    Engine::Serialize::ObjectIdentifier resolvedIdentifier;
    ASSERT_TRUE(Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
        restored->GetInstanceID(),
        resolvedIdentifier));
    EXPECT_EQ(resolvedIdentifier, identifier);
}

TEST(HierarchyUndoTests, DuplicateSubtreeRoundTripsWithStableIdsAndParentOrder)
{
    Undo::EditorUndoService undo;
    Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("Parent");
    auto& source = scene.CreateGameObject("Source");
    auto& sourceChild = scene.CreateGameObject("Source Child");
    auto& last = scene.CreateGameObject("Last");
    source.SetParent(parent);
    sourceChild.SetParent(source);
    last.SetParent(parent);

    ASSERT_TRUE(DuplicateGameObject(undo, source, nullptr, SceneScope("duplicate-subtree-scene")).Succeeded());
    ASSERT_EQ(parent.GetChildren().size(), 3u);
    auto* duplicate = parent.GetChildren()[2];
    ASSERT_NE(duplicate, nullptr);
    ASSERT_NE(duplicate, &source);
    const auto duplicateId = duplicate->GetInstanceID();
    ASSERT_EQ(duplicate->GetChildren().size(), 1u);
    const auto duplicateChildId = duplicate->GetChildren().front()->GetInstanceID();
    EXPECT_NE(duplicateId, source.GetInstanceID());
    EXPECT_NE(duplicateChildId, sourceChild.GetInstanceID());
    ExpectChildren(parent, {&source, &last, duplicate});

    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    ExpectChildren(parent, {&source, &last});
    EXPECT_EQ(Object::IDToPointer(duplicateId), nullptr);
    EXPECT_EQ(Object::IDToPointer(duplicateChildId), nullptr);

    ASSERT_EQ(undo.RequestRedo().state, Undo::UndoRequestState::Completed);
    auto* restoredDuplicate = Object::IDToPointer<Engine::GameObject>(duplicateId);
    auto* restoredChild = Object::IDToPointer<Engine::GameObject>(duplicateChildId);
    ASSERT_NE(restoredDuplicate, nullptr);
    ASSERT_NE(restoredChild, nullptr);
    ExpectChildren(parent, {&source, &last, restoredDuplicate});
    EXPECT_EQ(restoredChild->GetParent(), restoredDuplicate);
    Inspector::EditorSelectionService::Get().Clear();
}

TEST(HierarchyUndoTests, DeleteUndoReportsConflictWhenParentChangesExternally)
{
    Undo::EditorUndoService undo;
    Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("Parent");
    auto& deleted = scene.CreateGameObject("Deleted");
    auto& external = scene.CreateGameObject("External");
    deleted.SetParent(parent);
    const auto deletedId = deleted.GetInstanceID();

    ASSERT_TRUE(DeleteGameObject(undo, deleted, SceneScope("delete-conflict-scene")).Succeeded());
    external.SetParent(parent);

    const auto result = undo.RequestUndo();
    EXPECT_EQ(result.state, Undo::UndoRequestState::Conflict);
    EXPECT_EQ(undo.GetCursor(), 1u);
    EXPECT_EQ(Object::IDToPointer(deletedId), nullptr);
    ExpectChildren(parent, {&external});
}
} // namespace NLS::Editor::Panels::HierarchyUndo::Tests
