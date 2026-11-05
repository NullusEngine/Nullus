#include <gtest/gtest.h>
#include <algorithm>

#include <Components/TransformComponent.h>
#include <GameObject.h>
#include <ServiceLocator.h>

#include "Inspector/InspectorPropertyEditor.h"
#include "Inspector/InspectorProjectionBuilder.h"
#include "Inspector/InspectorRegistry.h"
#include "Serialization/AccessorSerializedBackend.h"
#include "Undo/EditorUndoService.h"
#include <imgui.h>

namespace
{
using namespace NLS;
using namespace NLS::Editor;

class InspectorLivePreviewTests : public testing::Test
{
protected:
    void SetUp() override { NLS::Core::ServiceLocator::Provide(undo); }
    void TearDown() override { NLS::Core::ServiceLocator::Remove<Undo::EditorUndoService>(); }
    Serialization::SerializedTargetSet Targets(bool multi = false)
    {
        Serialization::SerializedTargetSet targets;
        targets.targets.push_back(Inspector::InspectorTargetHandle::FromObject(
            *first.GetTransform(), Inspector::InspectorTargetKind::NativeObject));
        if (multi)
            targets.targets.push_back(Inspector::InspectorTargetHandle::FromObject(
                *second.GetTransform(), Inspector::InspectorTargetKind::NativeObject));
        targets.Normalize();
        return targets;
    }
    Undo::EditorUndoService undo;
    Engine::GameObject first {"First"};
    Engine::GameObject second {"Second"};
};

TEST_F(InspectorLivePreviewTests, PreviewWritesNativeTransformBeforeCommitAndRecordsOneUndo)
{
    Serialization::SerializedObject object(Targets(), Serialization::AccessorSerializedBackend::Create());
    const auto position = object.FindProperty("localPosition");
    ASSERT_TRUE(position);
    ASSERT_TRUE(position.SetValue(meta::Variant(Maths::Vector3 {1, 2, 3})));
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), Maths::Vector3::Zero);
    ASSERT_TRUE(object.PreviewModifiedProperties()) << object.GetLastError();
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), (Maths::Vector3 {1, 2, 3}));
    EXPECT_EQ(undo.GetHistoryState().transactionCount, 0u);
    EXPECT_FALSE(object.UpdateIfRequiredOrScript());
    ASSERT_TRUE(position.SetValue(meta::Variant(Maths::Vector3 {4, 5, 6})));
    ASSERT_TRUE(object.PreviewModifiedProperties()) << object.GetLastError();
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), (Maths::Vector3 {4, 5, 6}));
    ASSERT_TRUE(object.ApplyModifiedProperties()) << object.GetLastError();
    EXPECT_FALSE(undo.HasActiveEditSession());
    EXPECT_EQ(undo.GetHistoryState().transactionCount, 1u);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), Maths::Vector3::Zero);
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), (Maths::Vector3 {4, 5, 6}));
}

TEST_F(InspectorLivePreviewTests, DiscardRestoresThePreviewWithoutHistory)
{
    Serialization::SerializedObject object(Targets(), Serialization::AccessorSerializedBackend::Create());
    ASSERT_TRUE(object.FindProperty("localPosition").SetValue(meta::Variant(Maths::Vector3 {7, 8, 9})));
    ASSERT_TRUE(object.PreviewModifiedProperties()) << object.GetLastError();
    object.DiscardModifiedProperties();
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), Maths::Vector3::Zero);
    EXPECT_FALSE(object.HasModifiedProperties());
    EXPECT_FALSE(undo.HasActiveEditSession());
    EXPECT_EQ(undo.GetHistoryState().transactionCount, 0u);
}

TEST_F(InspectorLivePreviewTests, ReturningToTheStartingValueRestoresNativeAndDoesNotRecordHistory)
{
    Serialization::SerializedObject object(Targets(), Serialization::AccessorSerializedBackend::Create());
    const auto position = object.FindProperty("localPosition");
    ASSERT_TRUE(position.SetValue(meta::Variant(Maths::Vector3 {7, 8, 9})));
    ASSERT_TRUE(object.PreviewModifiedProperties());
    ASSERT_TRUE(position.SetValue(meta::Variant(Maths::Vector3::Zero)));
    ASSERT_TRUE(object.PreviewModifiedProperties()) << object.GetLastError();
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), Maths::Vector3::Zero);
    ASSERT_TRUE(object.ApplyModifiedProperties());
    EXPECT_EQ(undo.GetHistoryState().transactionCount, 0u);
    EXPECT_FALSE(undo.HasActiveEditSession());
}

TEST_F(InspectorLivePreviewTests, MultiObjectComponentPreviewPreservesOtherAxesAndUndoRestoresBoth)
{
    first.GetTransform()->SetLocalPosition({1, 2, 3});
    second.GetTransform()->SetLocalPosition({4, 5, 6});
    Serialization::SerializedObject object(Targets(true), Serialization::AccessorSerializedBackend::Create());
    const auto position = object.FindProperty("localPosition");
    ASSERT_TRUE(position.SetValueComponents(meta::Variant(Maths::Vector3 {10, 99, 99}), 1u));
    ASSERT_TRUE(object.PreviewModifiedProperties()) << object.GetLastError();
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), (Maths::Vector3 {10, 2, 3}));
    EXPECT_EQ(second.GetTransform()->GetLocalPosition(), (Maths::Vector3 {10, 5, 6}));
    ASSERT_TRUE(object.ApplyModifiedProperties());
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), (Maths::Vector3 {1, 2, 3}));
    EXPECT_EQ(second.GetTransform()->GetLocalPosition(), (Maths::Vector3 {4, 5, 6}));
}

TEST_F(InspectorLivePreviewTests, DestroyingOrRebindingTheInspectorRestoresUncommittedPreview)
{
    {
        Serialization::SerializedObject object(Targets(), Serialization::AccessorSerializedBackend::Create());
        ASSERT_TRUE(object.FindProperty("localPosition").SetValue(meta::Variant(Maths::Vector3 {3, 2, 1})));
        ASSERT_TRUE(object.PreviewModifiedProperties());
    }
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), Maths::Vector3::Zero);
    EXPECT_FALSE(undo.HasActiveEditSession());
    Serialization::SerializedObject object(Targets(), Serialization::AccessorSerializedBackend::Create());
    ASSERT_TRUE(object.FindProperty("localPosition").SetValue(meta::Variant(Maths::Vector3 {3, 2, 1})));
    ASSERT_TRUE(object.PreviewModifiedProperties());
    object.Reset(Targets(true), Serialization::AccessorSerializedBackend::Create());
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), Maths::Vector3::Zero);
    EXPECT_FALSE(undo.HasActiveEditSession());
}

TEST_F(InspectorLivePreviewTests, StagedSessionsRemainDeferredUnlessLivePreviewIsRequested)
{
    Serialization::SerializedObject object(Targets(), Serialization::AccessorSerializedBackend::Create());
    auto session = object.BeginEditSession("localPosition");
    ASSERT_TRUE(session.Preview(meta::Variant(Maths::Vector3 {3, 2, 1})));
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), Maths::Vector3::Zero);
    ASSERT_TRUE(session.Commit());
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), (Maths::Vector3 {3, 2, 1}));
}

TEST_F(InspectorLivePreviewTests, InspectorControlPreviewsWhileActiveAndCommitsOnRelease)
{
    const auto previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    struct Cleanup
    {
        ImGuiContext* gui;
        ImGuiContext* previous;
        ~Cleanup() { ImGui::DestroyContext(gui); ImGui::SetCurrentContext(previous); }
    } cleanup {gui, previous};
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800, 600};
    io.DeltaTime = 1.f / 60;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Serialization::SerializedObject object(Targets(), Serialization::AccessorSerializedBackend::Create());
    auto properties = Inspector::InspectorProjectionBuilder::BuildPropertyProjection(object, Targets(), {});
    auto found = std::find_if(properties.begin(), properties.end(), [](const auto& property)
        { return property.path.ToString() == "localPosition"; });
    ASSERT_NE(found, properties.end());
    found->metadata.customDrawer = "live-transform-preview";
    auto release = std::make_shared<bool>(false);
    Inspector::PropertyDrawerRegistration drawer;
    drawer.scope = Inspector::InspectorDrawerScope::ExplicitMetadata;
    drawer.identifier = "InspectorLivePreviewTests.control";
    drawer.metadataKey = "live-transform-preview";
    drawer.draw = [release](Inspector::InspectorPropertyDrawerContext& context)
    {
        ImGui::TextUnformatted("Position drag");
        context.changed = context.preview(meta::Variant(Maths::Vector3 {4, 5, 6}), "Move");
        if (*release) (void)context.commit();
        return true;
    };
    Inspector::InspectorRegistry::Get().RegisterPropertyDrawer(std::move(drawer));
    Inspector::InspectorPropertyEditor editor;
    Inspector::PropertyEditorContext context;
    context.object = &object;
    context.livePreview = true;
    const auto draw = [&]
    {
        ImGui::NewFrame();
        ImGui::Begin("Live Inspector");
        auto result = editor.DrawProperty(*found, context);
        ImGui::End();
        ImGui::Render();
        return result;
    };
    EXPECT_FALSE(draw().completed);
    EXPECT_EQ(first.GetTransform()->GetLocalPosition(), (Maths::Vector3 {4, 5, 6}));
    EXPECT_EQ(undo.GetHistoryState().transactionCount, 0u);
    *release = true;
    EXPECT_TRUE(draw().completed);
    EXPECT_EQ(undo.GetHistoryState().transactionCount, 1u);
}
} // namespace
