#include <gtest/gtest.h>
#include <array>
#include <Engine/GameObject.h>
#include <Engine/Components/TransformComponent.h>
#include <Engine/SceneSystem/Scene.h>
#include "Core/SceneSelection.h"
#include "Inspector/EditorSelectionService.h"
#include "Panels/HierarchyUndo.h"
#include "Panels/SceneViewPickingPolicy.h"
#include "Inspector/InspectorView.h"
#include "Properties/PropertyDrawerRegistry.h"
#include <ImGui/imgui.h>
#include <Math/ExternalReflection.h>
#include <cmath>
#include <ServiceLocator.h>
#include "Rendering/DebugGameObjectSelectionCollector.h"
#include <Engine/Components/CameraComponent.h>
#include <UI/Widgets/Layout/TreeNode.h>

namespace NLS::Editor::Tests
{
using namespace Inspector;
namespace
{
InspectorTargetHandle Handle(Engine::GameObject& object)
{ return InspectorTargetHandle::FromObject(object, InspectorTargetKind::GameObject); }
Undo::UndoScopeId Scope() { return {Undo::UndoScopeKind::Scene, "multi-selection-tests", 1}; }
class SceneSelectionTests : public ::testing::Test
{
protected:
    void SetUp() override { EditorSelectionService::Get().Clear(); }
    void TearDown() override { EditorSelectionService::Get().Clear(); }
};
struct UndoServiceScope
{
    Undo::EditorUndoService undo;
    UndoServiceScope() { NLS::Core::ServiceLocator::Provide(undo); }
    ~UndoServiceScope() { NLS::Core::ServiceLocator::Remove<Undo::EditorUndoService>(); }
};
}

TEST_F(SceneSelectionTests, ActiveObjectDoesNotChangeSingleTargetAccessor)
{
    Engine::GameObject a("A"), b("B");
    InspectorTargetSet set{{Handle(a), Handle(b)}};
    set.activeTarget = Handle(a); set.Normalize();
    EXPECT_EQ(set.Primary(), nullptr);
    ASSERT_NE(set.Active(), nullptr);
    EXPECT_EQ(*set.Active(), Handle(a));
    set.targets.erase(set.targets.begin()); set.Normalize();
    EXPECT_EQ(*set.Active(), Handle(b));
    EXPECT_EQ(set.Primary(), set.Active());
}

TEST_F(SceneSelectionTests, OperationsKeepOrderAndPreserveMarqueeBaseline)
{
    Engine::GameObject a("A"), b("B"), c("C");
    InspectorTargetSet before{{Handle(a), Handle(b)}};
    before.activeTarget = Handle(a); before.Normalize();
    auto added = ComposeSelection(before, {{Handle(b), Handle(c)}}, SelectionOperation::Add, true);
    ASSERT_EQ(added.targets.size(), 3u); EXPECT_EQ(added.targets[0], Handle(a));
    EXPECT_EQ(*added.Active(), Handle(a));
    auto shrunk = ComposeSelection(before, {{Handle(b)}}, SelectionOperation::Add, true);
    EXPECT_EQ(shrunk, before);
    auto toggled = ComposeSelection(before, {{Handle(b), Handle(c)}}, SelectionOperation::Toggle);
    ASSERT_EQ(toggled.targets.size(), 2u); EXPECT_EQ(toggled.targets.back(), Handle(c));
    EXPECT_EQ(*toggled.Active(), Handle(c));
    auto removed = ComposeSelection(before, {{Handle(a)}}, SelectionOperation::Remove);
    EXPECT_EQ(*removed.Active(), Handle(b));
}

TEST_F(SceneSelectionTests, ActiveOnlyChangesPublishExactlyOnceAndSelectionHasNoUndo)
{
    Engine::GameObject a("A"), b("B");
    Undo::EditorUndoService undo;
    auto& service = EditorSelectionService::Get();
    service.SetSelection({{Handle(a), Handle(b)}});
    const auto revision = service.GetSelection().revision;
    int events = 0;
    const auto listener = service.SelectionChangedEvent += [&](const auto&) { ++events; };
    service.SetActive(Handle(a)); service.SetActive(Handle(a));
    EXPECT_EQ(events, 1); EXPECT_GT(service.GetSelection().revision, revision);
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    service.SelectionChangedEvent -= listener;
}

TEST_F(SceneSelectionTests, BatchedUpdatesPublishOnlyTheFinalSelection)
{
    Engine::GameObject a("A"), b("B");
    auto& service = EditorSelectionService::Get();
    int events = 0;
    const auto listener = service.SelectionChangedEvent += [&](const auto&) { ++events; };
    {
        EditorSelectionUpdate update;
        service.Select(Handle(a)); service.ApplySelection({{Handle(b)}}, SelectionOperation::Add); service.SetActive(Handle(a));
        EXPECT_EQ(events, 0);
    }
    EXPECT_EQ(events, 1); EXPECT_EQ(service.GetSelection().targets.size(), 2u);
    EXPECT_EQ(*service.GetSelection().Active(), Handle(a));
    service.SelectionChangedEvent -= listener;
}

TEST_F(SceneSelectionTests, ReentrantSelectionPublishesImmutableSnapshotsInOrder)
{
    Engine::GameObject a("A"), b("B");
    auto& service = EditorSelectionService::Get();
    service.SetSelection({{Handle(a), Handle(b)}});
    std::vector<InstanceID> received;
    const auto first = service.SelectionChangedEvent += [&](const auto& selection)
    { if (selection.Active() && selection.Active()->instanceId == a.GetInstanceID()) service.SetActive(Handle(b)); };
    const auto second = service.SelectionChangedEvent += [&](const auto& selection)
    { if (selection.Active()) received.push_back(selection.Active()->instanceId); };
    service.SetActive(Handle(a));
    EXPECT_EQ(received, (std::vector<InstanceID>{a.GetInstanceID(), b.GetInstanceID()}));
    service.SelectionChangedEvent -= first; service.SelectionChangedEvent -= second;
}

TEST_F(SceneSelectionTests, WorkingSceneSelectionDropsAssetsAndOtherScenes)
{
    Engine::SceneSystem::Scene first, second;
    auto& a = first.CreateGameObject("A"); auto& b = second.CreateGameObject("B");
    InspectorTargetSet selection{{Handle(a), Handle(b), InspectorTargetHandle::FromAsset(InspectorTargetKind::Material, "Assets/material.mat")}};
    selection.activeTarget = Handle(b);
    const auto filtered = Core::FilterSceneSelection(selection, first);
    ASSERT_EQ(filtered.targets.size(), 1u); EXPECT_EQ(filtered.targets.front(), Handle(a));
    ASSERT_NE(filtered.Active(), nullptr); EXPECT_EQ(*filtered.Active(), Handle(a));
    EXPECT_EQ(selection.targets.size(), 3u);
}

TEST_F(SceneSelectionTests, RootsDropDuplicatesAndSelectedDescendants)
{
    Engine::SceneSystem::Scene scene;
    auto& a = scene.CreateGameObject("A"); auto& b = scene.CreateGameObject("B");
    auto& child = scene.CreateGameObject("Child"); child.SetParent(a);
    std::array objects{&child, &b, &a, &a};
    const auto roots = Core::GetSceneSelectionRoots(objects);
    ASSERT_EQ(roots.size(), 2u); EXPECT_EQ(roots[0], &b); EXPECT_EQ(roots[1], &a);
}

TEST_F(SceneSelectionTests, SelectionCenterIncludesObjectsWithoutGeometry)
{
    Engine::GameObject a("A"), b("B"), c("C");
    a.GetTransform()->SetWorldPosition({0, 0, 0});
    b.GetTransform()->SetWorldPosition({3, 0, 0});
    c.GetTransform()->SetWorldPosition({12, 0, 0});
    std::array objects{&a, &b, &c};
    const auto bounds = Core::GetSceneSelectionBounds(objects);
    ASSERT_TRUE(bounds);
    EXPECT_FLOAT_EQ(bounds->min.x, 0);
    EXPECT_FLOAT_EQ(bounds->max.x, 12);
    const auto handle = Core::BuildSceneSelectionGizmoMatrix(objects, b,
        Core::SceneViewGizmoPivot::Center, Core::SceneViewGizmoSpace::Global);
    EXPECT_FLOAT_EQ(handle.data[3], 6);
    EXPECT_FALSE(Core::GetSceneSelectionBounds(objects, false));
}

TEST_F(SceneSelectionTests, ProjectedBoundsCrossNearPlaneAndRespectViewportOffset)
{
    const Core::SceneSelectionRect viewport{{100, 50}, {900, 650}};
    auto projected = Core::ProjectSceneSelectionBounds({{-.5f,-.5f,-2}, {.5f,.5f,0}}, Maths::Matrix4::Identity, viewport);
    ASSERT_TRUE(projected); EXPECT_FLOAT_EQ(projected->min.x, 300); EXPECT_FLOAT_EQ(projected->max.y, 500);
    EXPECT_FALSE(Core::ProjectSceneSelectionBounds({{-.5f,-.5f,-3}, {.5f,.5f,-2}}, Maths::Matrix4::Identity, viewport));
    EXPECT_TRUE(Core::MakeSceneSelectionRect({600,400}, {200,100}).Intersects(*projected));
    EXPECT_FALSE(Core::MakeSceneSelectionRect({100,50}, {200,100}).Intersects(*projected));
}

TEST_F(SceneSelectionTests, ProjectedBoundsClipPerspectiveAndOrthographicNearPlanes)
{
    const Core::SceneSelectionRect viewport{{40, 80}, {840, 880}};
    const auto perspective = Maths::Matrix4::CreatePerspective(60, 1, 1, 100);
    const auto orthographic = Maths::Matrix4::CreateOrthographic(4, 1, 1, 100);
    const Core::SceneSelectionBounds crossing{{-.25f, -.25f, -2}, {.25f, .25f, -.5f}};
    const Core::SceneSelectionBounds beforeNear{{-.25f, -.25f, -.8f}, {.25f, .25f, -.2f}};
    ASSERT_TRUE(Core::ProjectSceneSelectionBounds(crossing, perspective, viewport));
    ASSERT_TRUE(Core::ProjectSceneSelectionBounds(crossing, orthographic, viewport));
    EXPECT_FALSE(Core::ProjectSceneSelectionBounds(beforeNear, perspective, viewport));
    EXPECT_FALSE(Core::ProjectSceneSelectionBounds(beforeNear, orthographic, viewport));
}

TEST_F(SceneSelectionTests, PendingClickCanResolveAfterPointerLeavesViewport)
{
    EXPECT_TRUE(Panels::ShouldRenderScenePickingFrame(false, false, false, true, false, false, false, false, false, true));
    EXPECT_FALSE(Panels::ShouldRenderScenePickingFrame(false, false, false, false, false, false, false, false, false, true));
}

TEST_F(SceneSelectionTests, BulkDeleteRestoresSiblingOrderSelectionAndActiveObject)
{
    Engine::SceneSystem::Scene scene; Undo::EditorUndoService undo;
    auto& parent = scene.CreateGameObject("Parent");
    auto& a = scene.CreateGameObject("A"); auto& b = scene.CreateGameObject("B");
    auto& c = scene.CreateGameObject("C"); auto& child = scene.CreateGameObject("Child");
    a.SetParent(parent); b.SetParent(parent); c.SetParent(parent); child.SetParent(a);
    const auto aId = a.GetInstanceID(), bId = b.GetInstanceID(), childId = child.GetInstanceID();
    InspectorTargetSet selected{{Handle(a), Handle(child), Handle(b)}}; selected.activeTarget = Handle(child);
    EditorSelectionService::Get().SetSelection(selected);
    std::array objects{&a, &child, &b};
    ASSERT_TRUE(Panels::HierarchyUndo::DeleteGameObjects(undo, objects, Scope()).Succeeded());
    ASSERT_EQ(parent.GetChildren().size(), 1u); EXPECT_EQ(parent.GetChildren()[0], &c);
    EXPECT_TRUE(EditorSelectionService::Get().GetSelection().Empty()); EXPECT_EQ(undo.GetTransactionCount(), 1u);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    ASSERT_EQ(parent.GetChildren().size(), 3u);
    EXPECT_EQ(parent.GetChildren()[0]->GetInstanceID(), aId); EXPECT_EQ(parent.GetChildren()[1]->GetInstanceID(), bId);
    const auto& restored = EditorSelectionService::Get().GetSelection();
    ASSERT_EQ(restored.targets.size(), 3u); ASSERT_NE(restored.Active(), nullptr); EXPECT_EQ(restored.Active()->instanceId, childId);
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); EXPECT_EQ(parent.GetChildren().size(), 1u);
}

TEST_F(SceneSelectionTests, BulkDeleteRestoresSceneRootOrder)
{
    Engine::SceneSystem::Scene scene; Undo::EditorUndoService undo;
    auto& a = scene.CreateGameObject("A"); auto& b = scene.CreateGameObject("B");
    auto& c = scene.CreateGameObject("C"); auto& d = scene.CreateGameObject("D");
    const auto aId = a.GetInstanceID(), bId = b.GetInstanceID();
    const auto cId = c.GetInstanceID(), dId = d.GetInstanceID();
    const std::array selected{&b, &d};
    ASSERT_TRUE(Panels::HierarchyUndo::DeleteGameObjects(undo, selected, Scope()).Succeeded());
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    ASSERT_EQ(scene.GetGameObjects().size(), 4u);
    EXPECT_EQ(scene.GetGameObjects()[0]->GetInstanceID(), aId);
    EXPECT_EQ(scene.GetGameObjects()[1]->GetInstanceID(), bId);
    EXPECT_EQ(scene.GetGameObjects()[2]->GetInstanceID(), cId);
    EXPECT_EQ(scene.GetGameObjects()[3]->GetInstanceID(), dId);
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    ASSERT_EQ(scene.GetGameObjects().size(), 2u);
    EXPECT_EQ(scene.GetGameObjects()[0]->GetInstanceID(), aId);
    EXPECT_EQ(scene.GetGameObjects()[1]->GetInstanceID(), cId);
}

TEST_F(SceneSelectionTests, HierarchyWidgetOrderChangesWithoutLosingState)
{
    UI::Widgets::TreeNode root("Root");
    auto& a = root.CreateWidget<UI::Widgets::TreeNode>("A");
    auto& b = root.CreateWidget<UI::Widgets::TreeNode>("B");
    a.selected = true;
    std::array<UI::Widgets::AWidget*, 2> ordered{&b, &a};
    ASSERT_TRUE(root.ReorderChildren(ordered));
    EXPECT_EQ(root.GetWidgets()[0].first, &b);
    EXPECT_EQ(root.GetWidgets()[1].first, &a);
    EXPECT_TRUE(a.selected);
    std::array<UI::Widgets::AWidget*, 2> invalid{&a, &a};
    EXPECT_FALSE(root.ReorderChildren(invalid));
    EXPECT_EQ(root.GetWidgets()[0].first, &b);
}

TEST_F(SceneSelectionTests, BulkDuplicateMapsExplicitDescendantsAndActiveObject)
{
    Engine::SceneSystem::Scene scene; Undo::EditorUndoService undo;
    auto& parent = scene.CreateGameObject("Parent"); auto& a = scene.CreateGameObject("A");
    auto& child = scene.CreateGameObject("Child"); auto& b = scene.CreateGameObject("B");
    a.SetParent(parent); child.SetParent(a); b.SetParent(parent);
    InspectorTargetSet selected{{Handle(a), Handle(child), Handle(b)}}; selected.activeTarget = Handle(child);
    EditorSelectionService::Get().SetSelection(selected);
    std::array objects{&a, &child, &b};
    ASSERT_TRUE(Panels::HierarchyUndo::DuplicateGameObjects(undo, objects, Scope()).Succeeded());
    ASSERT_EQ(parent.GetChildren().size(), 4u); EXPECT_EQ(undo.GetTransactionCount(), 1u);
    const auto selection = EditorSelectionService::Get().GetSelection();
    ASSERT_EQ(selection.targets.size(), 3u); ASSERT_NE(selection.Active(), nullptr);
    EXPECT_EQ(selection.Active()->instanceId, parent.GetChildren()[2]->GetChildren()[0]->GetInstanceID());
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_EQ(parent.GetChildren().size(), 2u);
    EXPECT_EQ(EditorSelectionService::Get().GetSelection(), selected);
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); EXPECT_EQ(parent.GetChildren().size(), 4u);
    EXPECT_EQ(EditorSelectionService::Get().GetSelection(), selection);
}

TEST_F(SceneSelectionTests, BulkReparentKeepsWorldTransformAndUndoOrder)
{
    Engine::SceneSystem::Scene scene; Undo::EditorUndoService undo;
    auto& oldParent = scene.CreateGameObject("Old"); auto& destination = scene.CreateGameObject("New");
    oldParent.GetTransform()->SetLocalPosition({10,0,0}); destination.GetTransform()->SetLocalPosition({-20,0,0});
    auto& a = scene.CreateGameObject("A"); auto& b = scene.CreateGameObject("B"); auto& c = scene.CreateGameObject("C");
    a.SetParent(oldParent); b.SetParent(oldParent); c.SetParent(oldParent);
    const auto position = a.GetTransform()->GetWorldPosition();
    std::array objects{&a, &b};
    ASSERT_TRUE(Panels::HierarchyUndo::ReparentGameObjects(undo, objects, &destination, Scope()).Succeeded());
    EXPECT_EQ(a.GetTransform()->GetWorldPosition(), position); EXPECT_EQ(destination.GetChildren().size(), 2u);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); ASSERT_EQ(oldParent.GetChildren().size(), 3u);
    EXPECT_EQ(oldParent.GetChildren()[0], &a); EXPECT_EQ(oldParent.GetChildren()[1], &b); EXPECT_EQ(oldParent.GetChildren()[2], &c);
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); EXPECT_EQ(destination.GetChildren().size(), 2u);
}

TEST_F(SceneSelectionTests, BatchRejectsCyclesAndCrossSceneBeforeMutation)
{
    Engine::SceneSystem::Scene scene, other; Undo::EditorUndoService undo;
    auto& a = scene.CreateGameObject("A"); auto& child = scene.CreateGameObject("Child"); child.SetParent(a);
    auto& b = scene.CreateGameObject("B"); auto& external = other.CreateGameObject("External");
    std::array roots{&a, &b};
    EXPECT_FALSE(Panels::HierarchyUndo::ReparentGameObjects(undo, roots, &child, Scope()).Succeeded());
    EXPECT_FALSE(Panels::HierarchyUndo::ReparentGameObjects(undo, roots, &external, Scope()).Succeeded());
    std::array mixed{&a, &external};
    EXPECT_FALSE(Panels::HierarchyUndo::DeleteGameObjects(undo, mixed, Scope()).Succeeded());
    EXPECT_EQ(child.GetParent(), &a); EXPECT_EQ(undo.GetTransactionCount(), 0u);
}

TEST_F(SceneSelectionTests, BulkUndoRejectsExternalParentChangesAtomically)
{
    Engine::SceneSystem::Scene scene; Undo::EditorUndoService undo;
    auto& parent = scene.CreateGameObject("Parent"); auto& a = scene.CreateGameObject("A");
    auto& b = scene.CreateGameObject("B"); a.SetParent(parent); b.SetParent(parent);
    std::array objects{&a, &b};
    ASSERT_TRUE(Panels::HierarchyUndo::DuplicateGameObjects(undo, objects, Scope()).Succeeded());
    auto& intervening = scene.CreateGameObject("Outside Timeline"); intervening.SetParent(parent);
    EXPECT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Conflict);
    EXPECT_EQ(parent.GetChildren().size(), 5u);
}

TEST_F(SceneSelectionTests, ExplicitlySelectedChildHasAnOutlineAndIsCollectedOnlyOnce)
{
    Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("Parent"); auto& child = scene.CreateGameObject("Child");
    auto& other = scene.CreateGameObject("Other");
    child.SetParent(parent);
    ASSERT_NE(child.AddComponent<Engine::Components::CameraComponent>(), nullptr);
    ASSERT_NE(other.AddComponent<Engine::Components::CameraComponent>(), nullptr);
    std::array objects{&parent, &child, &other};
    Rendering::DebugGameObjectDebugDrawItems items;
    Rendering::CollectSelectedDebugGameObjectsDebugDrawItems(objects, items);
    EXPECT_EQ(items.visitedGameObjects, 3u);
    ASSERT_EQ(items.cameras.size(), 2u);
    EXPECT_EQ(items.cameras[0].selectionClassification, Rendering::kSelectionOutlineParentClassification);
    EXPECT_NE(items.cameras[0].selectionGroupId, items.cameras[1].selectionGroupId);
}

TEST_F(SceneSelectionTests, DragThresholdNeverTurnsAnExistingMarqueeBackIntoAClick)
{
    Core::SceneSelectionDrag drag{{10,20},{10,20}};
    drag.Update({12,22}, 6); EXPECT_FALSE(drag.marquee);
    drag.Update({30,20}, 6); EXPECT_TRUE(drag.marquee);
    drag.Update({10,20}, 6); EXPECT_TRUE(drag.marquee);
}

TEST_F(SceneSelectionTests, GroupTranslationIsRelativeToTheInitialPose)
{
    Core::SceneSelectionTransform before{{1,2,3}, {}, {-2,3,4}};
    const auto result = Core::TransformSceneSelection(before, Maths::Matrix4::Identity,
        Maths::Matrix4::Translation({5,-1,2}), Core::EGizmoOperation::TRANSLATE,
        Core::SceneViewGizmoPivot::Pivot, Core::SceneViewGizmoSpace::Global);
    EXPECT_EQ(result.worldPosition, Maths::Vector3(6,1,5)); EXPECT_EQ(result.localScale, before.localScale);
    const auto repeated = Core::TransformSceneSelection(before, Maths::Matrix4::Identity,
        Maths::Matrix4::Translation({5,-1,2}), Core::EGizmoOperation::TRANSLATE,
        Core::SceneViewGizmoPivot::Pivot, Core::SceneViewGizmoSpace::Global);
    EXPECT_EQ(repeated.worldPosition, result.worldPosition);
}

TEST_F(SceneSelectionTests, CenterRotationOrbitsWhilePivotRotationKeepsEachPosition)
{
    const float sine = std::sqrt(.5f);
    const auto rotated = Maths::Quaternion::ToMatrix4({0,0,sine,sine});
    Core::SceneSelectionTransform before{{2,0,0}, {}, {1,1,1}};
    auto center = Core::TransformSceneSelection(before, Maths::Matrix4::Identity, rotated,
        Core::EGizmoOperation::ROTATE, Core::SceneViewGizmoPivot::Center, Core::SceneViewGizmoSpace::Global);
    EXPECT_NEAR(center.worldPosition.x, 0, 1.e-5f); EXPECT_NEAR(center.worldPosition.y, 2, 1.e-5f);
    auto pivot = Core::TransformSceneSelection(before, Maths::Matrix4::Identity, rotated,
        Core::EGizmoOperation::ROTATE, Core::SceneViewGizmoPivot::Pivot, Core::SceneViewGizmoSpace::Global);
    EXPECT_EQ(pivot.worldPosition, before.worldPosition);
}

TEST_F(SceneSelectionTests, LocalPivotRotationUsesEachObjectsOwnAxes)
{
    const float sine = std::sqrt(.5f);
    const Maths::Quaternion z{0,0,sine,sine}, x{sine,0,0,sine};
    Core::SceneSelectionTransform before{{2,0,0}, z, {1,1,1}};
    const auto start = Maths::Quaternion::ToMatrix4(z);
    const auto current = Maths::Quaternion::ToMatrix4(z * x);
    const auto result = Core::TransformSceneSelection(before, start, current,
        Core::EGizmoOperation::ROTATE, Core::SceneViewGizmoPivot::Pivot, Core::SceneViewGizmoSpace::Local);
    const auto up = result.worldRotation * Maths::Vector3{0,1,0};
    EXPECT_NEAR(up.x, 0, 1.e-5f); EXPECT_NEAR(up.y, 0, 1.e-5f); EXPECT_NEAR(up.z, 1, 1.e-5f);
    EXPECT_EQ(result.worldPosition, before.worldPosition);
}

TEST_F(SceneSelectionTests, CenterScaleChangesOffsetsAndPivotScalePreservesNegativeScale)
{
    Core::SceneSelectionTransform before{{1,2,3}, {}, {-2,3,4}};
    const auto scaled = Maths::Matrix4::Scaling({2,3,4});
    auto center = Core::TransformSceneSelection(before, Maths::Matrix4::Identity, scaled,
        Core::EGizmoOperation::SCALE, Core::SceneViewGizmoPivot::Center, Core::SceneViewGizmoSpace::Global);
    EXPECT_EQ(center.worldPosition, Maths::Vector3(2,6,12)); EXPECT_EQ(center.localScale, Maths::Vector3(-4,9,16));
    auto pivot = Core::TransformSceneSelection(before, Maths::Matrix4::Identity, scaled,
        Core::EGizmoOperation::SCALE, Core::SceneViewGizmoPivot::Pivot, Core::SceneViewGizmoSpace::Local);
    EXPECT_EQ(pivot.worldPosition, before.worldPosition); EXPECT_EQ(pivot.localScale, Maths::Vector3(-4,9,16));
}

TEST_F(SceneSelectionTests, ActualInspectorDrawKeepsTheSharedComponentStackAndEditedAxis)
{
    UndoServiceScope undoScope;
    auto& undo = undoScope.undo;
    auto* gui = ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = {1000,800}; ImGui::GetIO().IniFilename = nullptr;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Engine::SceneSystem::Scene scene;
    auto& a = scene.CreateGameObject("A"); auto& b = scene.CreateGameObject("B");
    a.GetTransform()->SetLocalPosition({1,2,3}); b.GetTransform()->SetLocalPosition({4,5,6});
    bool drawn = false, mixed = false, previewed = false;
    Properties::PropertyDrawerRegistration registration;
    registration.scope = InspectorDrawerScope::ExactType;
    registration.identifier = "SceneSelectionTests.Position";
    registration.valueType = NLS_TYPEOF(Maths::Vector3);
    registration.draw = [&](InspectorPropertyDrawerContext& context)
    {
        if (!context.serializedProperty || context.serializedProperty->GetPropertyPath() != "localPosition") return false;
        drawn = true;
        mixed = context.serializedProperty->HasMultipleDifferentValues();
        EXPECT_EQ(context.serializedProperty->GetTargetValues().size(), 2u);
        context.interaction = {true,false,true,true,false,1};
        previewed = context.preview(meta::Variant(Maths::Vector3{9,777,888}), "Edit Position X");
        context.changed = previewed;
        return true;
    };
    auto token = Properties::PropertyDrawerRegistry::Get().Register(std::move(registration));
    {
        InspectorView view(false); view.SetTargets({{Handle(a), Handle(b)}});
        ImGui::NewFrame(); ImGui::SetNextWindowSize({900,700}); ImGui::Begin("Multi Inspector");
        view.Draw(); ImGui::End(); ImGui::Render();
    }
    EXPECT_TRUE(drawn); EXPECT_TRUE(mixed); EXPECT_TRUE(previewed);
    EXPECT_EQ(a.GetTransform()->GetLocalPosition(), Maths::Vector3(9,2,3));
    EXPECT_EQ(b.GetTransform()->GetLocalPosition(), Maths::Vector3(9,5,6));
    token.Reset(); ImGui::DestroyContext(gui);
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_EQ(a.GetTransform()->GetLocalPosition(), Maths::Vector3(1,2,3));
    EXPECT_EQ(b.GetTransform()->GetLocalPosition(), Maths::Vector3(4,5,6));
}

TEST_F(SceneSelectionTests, ActualInspectorToolbarEditsAllTargetsAndRestoresEachValue)
{
    const std::array paths{"GameObject.active", "GameObject.name", "GameObject.tag", "GameObject.layer"};
    for (size_t index = 0; index < paths.size(); ++index)
    {
        SCOPED_TRACE(paths[index]);
        UndoServiceScope undoScope;
        auto* gui = ImGui::CreateContext();
        ImGui::GetIO().DisplaySize = {1000,800}; ImGui::GetIO().IniFilename = nullptr;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        Engine::SceneSystem::Scene scene;
        auto& a = scene.CreateGameObject("A"); auto& b = scene.CreateGameObject("B");
        b.SetActive(false); b.SetTag("Player"); b.SetLayer(5);
        const auto newValue = index == 0 ? meta::Variant(false) : index == 1 ? meta::Variant(std::string("Shared")) :
            index == 2 ? meta::Variant(std::string("Finish")) : meta::Variant(int32_t{8});
        bool drawn = false, mixed = false, previewed = false;
        Properties::PropertyDrawerRegistration registration;
        registration.scope = InspectorDrawerScope::ExactType;
        registration.identifier = std::string("SceneSelectionTests.Toolbar.") + paths[index];
        registration.valueType = index == 0 ? NLS_TYPEOF(bool) : index == 3 ? NLS_TYPEOF(int32_t) : NLS_TYPEOF(std::string);
        registration.draw = [&](InspectorPropertyDrawerContext& context)
        {
            if (context.property.path != PropertyPath(paths[index])) return false;
            drawn = true;
            EXPECT_NE(context.serializedProperty, nullptr);
            if (!context.serializedProperty) return true;
            mixed = context.serializedProperty->HasMultipleDifferentValues();
            EXPECT_EQ(context.serializedProperty->GetTargetValues().size(), 2u);
            context.interaction = {true,false,true,true,false,~uint64_t{0}};
            previewed = context.preview(newValue, "Edit shared header");
            context.changed = previewed;
            return true;
        };
        auto token = Properties::PropertyDrawerRegistry::Get().Register(std::move(registration));
        {
            InspectorView view(false); view.SetToolbarVisible(true); view.SetTargets({{Handle(a), Handle(b)}});
            ImGui::NewFrame(); ImGui::SetNextWindowSize({900,700}); ImGui::Begin("Multi Inspector Header");
            view.Draw(); ImGui::End(); ImGui::Render();
        }
        EXPECT_TRUE(drawn); EXPECT_TRUE(mixed); EXPECT_TRUE(previewed);
        EXPECT_EQ(undoScope.undo.GetTransactionCount(), 1u);
        if (index == 0) { EXPECT_FALSE(a.IsActive()); EXPECT_FALSE(b.IsActive()); }
        if (index == 1) { EXPECT_EQ(a.GetName(), "Shared"); EXPECT_EQ(b.GetName(), "Shared"); }
        if (index == 2) { EXPECT_EQ(a.GetTag(), "Finish"); EXPECT_EQ(b.GetTag(), "Finish"); }
        if (index == 3) { EXPECT_EQ(a.GetLayer(), 8); EXPECT_EQ(b.GetLayer(), 8); }
        ASSERT_TRUE(undoScope.undo.RequestUndo().Succeeded());
        EXPECT_TRUE(a.IsActive()); EXPECT_FALSE(b.IsActive());
        EXPECT_EQ(a.GetName(), "A"); EXPECT_EQ(b.GetName(), "B");
        EXPECT_EQ(a.GetTag(), "Untagged"); EXPECT_EQ(b.GetTag(), "Player");
        EXPECT_EQ(a.GetLayer(), 0); EXPECT_EQ(b.GetLayer(), 5);
        token.Reset(); ImGui::DestroyContext(gui);
    }
}
}
