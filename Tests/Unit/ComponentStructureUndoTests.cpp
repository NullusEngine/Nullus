#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "GameObject.h"
#include "Inspector/ComponentStructureUndo.h"
#include "Object/Object.h"
#include "Serialize/PPtr.h"
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Inspector::Tests
{
namespace
{
Undo::UndoCommandInfo MakeInfo()
{
    Undo::UndoCommandInfo info;
    info.label = "Change Components";
    info.sourceModule = "Inspector";
    info.scopes = {{Undo::UndoScopeKind::Scene, "scene:component-structure-test", 0u}};
    info.affected = {"component-structure-test"};
    return info;
}
} // namespace

TEST(ComponentStructureUndoTests, RestoresComponentSnapshotsThroughTheGlobalTimeline)
{
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    Engine::GameObject gameObject("Component structure Undo");
    auto* light = gameObject.AddComponent<Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetEnabled(false);
    const auto lightInstanceId = light->GetInstanceID();
    const auto before = CaptureComponentStructure(gameObject);
    ASSERT_EQ(before.components.size(), 2u);
    const auto lightIdentifier = before.components[1].objectIdentifier;
    ASSERT_TRUE(lightIdentifier.IsValid());

    auto* camera = gameObject.AddComponent<Engine::Components::CameraComponent>();
    ASSERT_NE(camera, nullptr);
    const auto cameraInstanceId = camera->GetInstanceID();
    const auto after = CaptureComponentStructure(gameObject);
    ASSERT_EQ(after.components.size(), 3u);
    const auto cameraIdentifier = after.components[2].objectIdentifier;
    ASSERT_TRUE(cameraIdentifier.IsValid());

    Undo::EditorUndoService undo;
    auto info = MakeInfo();
    const auto owner = CaptureComponentStructureOwner(gameObject, info);
    ASSERT_TRUE(owner.has_value());
    ASSERT_TRUE(undo.RecordApplied(MakeComponentStructureUndoCommand(
        std::move(info),
        *owner,
        before,
        after)).Succeeded());

    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(gameObject.GetComponent<Engine::Components::LightComponent>(), light);
    EXPECT_EQ(light->GetInstanceID(), lightInstanceId);
    EXPECT_EQ(gameObject.GetComponent<Engine::Components::CameraComponent>(), nullptr);
    ASSERT_FALSE(gameObject.GetComponents().empty());
    EXPECT_EQ(gameObject.GetComponents().front().get(), gameObject.GetTransform());

    ASSERT_EQ(undo.RequestRedo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(gameObject.GetComponent<Engine::Components::LightComponent>(), light);
    EXPECT_EQ(light->GetInstanceID(), lightInstanceId);
    EXPECT_FALSE(light->IsSelfEnabled());
    auto* restoredCamera = gameObject.GetComponent<Engine::Components::CameraComponent>();
    ASSERT_NE(restoredCamera, nullptr);
    EXPECT_EQ(restoredCamera->GetInstanceID(), cameraInstanceId);
    InstanceID mappedLight = InstanceID_None;
    InstanceID mappedCamera = InstanceID_None;
    ASSERT_TRUE(persistentManager.TryGetInstanceID(lightIdentifier, mappedLight));
    ASSERT_TRUE(persistentManager.TryGetInstanceID(cameraIdentifier, mappedCamera));
    EXPECT_EQ(mappedLight, light->GetInstanceID());
    EXPECT_EQ(mappedCamera, restoredCamera->GetInstanceID());
    EXPECT_EQ(NLS::Object::IDToPointer(mappedCamera), restoredCamera);
    ASSERT_FALSE(gameObject.GetComponents().empty());
    EXPECT_EQ(gameObject.GetComponents().front().get(), gameObject.GetTransform());

    persistentManager.Clear();
}

TEST(ComponentStructureUndoTests, DestroyedOwnerStopsAtTheStaleHistoryEntry)
{
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    Undo::EditorUndoService undo;
    {
        auto gameObject = std::make_unique<Engine::GameObject>("Destroyed component owner");
        const auto before = CaptureComponentStructure(*gameObject);
        ASSERT_NE(gameObject->AddComponent<Engine::Components::LightComponent>(), nullptr);
        const auto after = CaptureComponentStructure(*gameObject);
        auto info = MakeInfo();
        const auto owner = CaptureComponentStructureOwner(*gameObject, info);
        ASSERT_TRUE(owner.has_value());
        ASSERT_TRUE(undo.RecordApplied(MakeComponentStructureUndoCommand(
            std::move(info),
            *owner,
            before,
            after)).Succeeded());
    }

    EXPECT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Stale);
    EXPECT_FALSE(undo.CanUndo());

    persistentManager.Clear();
}

TEST(ComponentStructureUndoTests, ClearedOwnerIdentityNeverFallsBackToAReusedInstanceId)
{
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    Undo::EditorUndoService undo;
    InstanceID destroyedOwnerInstanceId = InstanceID_None;
    {
        auto gameObject = std::make_unique<Engine::GameObject>("Reused owner runtime slot");
        const auto before = CaptureComponentStructure(*gameObject);
        ASSERT_NE(gameObject->AddComponent<Engine::Components::LightComponent>(), nullptr);
        const auto after = CaptureComponentStructure(*gameObject);
        auto info = MakeInfo();
        const auto owner = CaptureComponentStructureOwner(*gameObject, info);
        ASSERT_TRUE(owner.has_value());
        destroyedOwnerInstanceId = gameObject->GetInstanceID();
        ASSERT_TRUE(undo.RecordApplied(MakeComponentStructureUndoCommand(
            std::move(info),
            *owner,
            before,
            after)).Succeeded());
    }

    // The old runtime slot can be occupied again, but the command no longer
    // has a PersistentManager mapping for its captured ObjectIdentifier.
    persistentManager.Clear();
    Engine::GameObject replacement("Unrelated owner");
    NLS::Object::AssignInstanceID(&replacement, destroyedOwnerInstanceId);
    ASSERT_EQ(replacement.GetInstanceID(), destroyedOwnerInstanceId);

    EXPECT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Stale);
    EXPECT_EQ(replacement.GetComponents().size(), 1u);

    persistentManager.Clear();
}

TEST(ComponentStructureUndoTests, EditSessionAppliesStructuralMutationBeforeRecordingTheStableCommand)
{
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    Engine::GameObject gameObject("Component structure edit session");
    auto* light = gameObject.AddComponent<Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    const auto lightInstanceId = light->GetInstanceID();
    const auto lightIdentifier = CaptureComponentObjectIdentifier(*light);
    ASSERT_TRUE(lightIdentifier.has_value());
    const auto before = CaptureComponentStructure(gameObject);

    ComponentStructureEditOperation operation;
    operation.kind = ComponentStructureEditKind::Remove;
    operation.componentIdentifier = *lightIdentifier;

    Undo::EditorUndoService undo;
    const auto owner = CaptureComponentStructureOwner(gameObject, MakeInfo());
    ASSERT_TRUE(owner.has_value());
    auto begin = undo.BeginEditSession({
        MakeInfo(),
        MakeComponentStructureUndoEdit(MakeInfo(), *owner, before, std::move(operation))});
    ASSERT_TRUE(begin.result.Succeeded());
    ASSERT_TRUE(begin.session.IsValid());

    auto session = begin.session;
    ASSERT_TRUE(undo.PreviewEditSession(session).Succeeded());
    EXPECT_EQ(gameObject.GetComponent<Engine::Components::LightComponent>(), nullptr);

    ASSERT_TRUE(undo.CommitEditSession(session).Succeeded());
    EXPECT_TRUE(undo.CanUndo());
    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    auto* restoredLight = gameObject.GetComponent<Engine::Components::LightComponent>();
    ASSERT_NE(restoredLight, nullptr);
    EXPECT_EQ(restoredLight->GetInstanceID(), lightInstanceId);

    ASSERT_EQ(undo.RequestRedo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(gameObject.GetComponent<Engine::Components::LightComponent>(), nullptr);

    persistentManager.Clear();
}

TEST(ComponentStructureUndoTests, ExecuteHelperMovesComponentsThroughOneGlobalEditSession)
{
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    persistentManager.Clear();

    Engine::GameObject gameObject("Component structure move session");
    auto* light = gameObject.AddComponent<Engine::Components::LightComponent>();
    auto* camera = gameObject.AddComponent<Engine::Components::CameraComponent>();
    ASSERT_NE(light, nullptr);
    ASSERT_NE(camera, nullptr);
    const auto before = CaptureComponentStructure(gameObject);

    ComponentStructureEditOperation operation;
    operation.kind = ComponentStructureEditKind::Move;
    const auto cameraIdentifier = CaptureComponentObjectIdentifier(*camera);
    ASSERT_TRUE(cameraIdentifier.has_value());
    operation.componentIdentifier = *cameraIdentifier;
    operation.destinationIndex = 1u;

    Undo::EditorUndoService undo;
    const auto owner = CaptureComponentStructureOwner(gameObject, MakeInfo());
    ASSERT_TRUE(owner.has_value());
    const auto executed = ExecuteComponentStructureEdit(
        undo,
        MakeInfo(),
        *owner,
        before,
        std::move(operation));
    ASSERT_TRUE(executed.Succeeded()) << executed.message;
    ASSERT_EQ(undo.GetTransactionCount(), 1u);
    ASSERT_EQ(gameObject.GetComponents().size(), 3u);
    EXPECT_EQ(gameObject.GetComponents()[1].get(), camera);
    EXPECT_EQ(gameObject.GetComponents()[2].get(), light);

    ASSERT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(gameObject.GetComponents()[1].get(), light);
    EXPECT_EQ(gameObject.GetComponents()[2].get(), camera);

    ASSERT_EQ(undo.RequestRedo().state, Undo::UndoRequestState::Completed);
    EXPECT_EQ(gameObject.GetComponents()[1].get(), camera);
    EXPECT_EQ(gameObject.GetComponents()[2].get(), light);
    EXPECT_EQ(gameObject.GetComponents().front().get(), gameObject.GetTransform());

    persistentManager.Clear();
}
} // namespace NLS::Editor::Inspector::Tests
