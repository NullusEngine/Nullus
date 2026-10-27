#include <gtest/gtest.h>
#include <array>
#include <imgui_internal.h>
#include "Properties/CollectionPropertyDrawer.h"
#include "Properties/PropertyPanel.h"
#include "Inspector/ReflectedPropertyEditor.h"
#include "Inspector/InspectorProjectionBuilder.h"
#include "Undo/EditorUndoService.h"
#include <ServiceLocator.h>
#include <Math/ExternalReflection.h>
#include <Serialize/ExternalReflection.h>
#include <Components/MeshRenderer.h>
#include "Serialization/NativeReflectionSerializedBackend.h"
#include "Inspector/InspectorView.h"

namespace NLS::Editor::Properties::Tests
{
struct CollectionRecord
{
    float number = 0;
    std::string title;
    std::vector<int> children;
};
struct UnalignedCollectionRecord
{
    bool marker = false;
    std::vector<std::string> items;
    uint64_t tail = 0;
};
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Properties::Tests::CollectionRecord)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Properties::Tests::UnalignedCollectionRecord)
inline void RegisterCollectionRecord(meta::ReflectionDatabase& db, meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Properties::Tests::CollectionRecord)
    metaExternalType.AddConstructor<MetaExternalType, false, false>({});
    NLS_META_EXTERNAL_FIELD(float, number);
    NLS_META_EXTERNAL_FIELD(std::string, title);
    NLS_META_EXTERNAL_FIELD(std::vector<int>, children);
    NLS_META_EXTERNAL_END();
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Properties::Tests::UnalignedCollectionRecord)
    metaExternalType.AddConstructor<MetaExternalType, false, false>({});
    NLS_META_EXTERNAL_FIELD(bool, marker);
    NLS_META_EXTERNAL_FIELD(std::vector<std::string>, items);
    NLS_META_EXTERNAL_FIELD(uint64_t, tail);
    NLS_META_EXTERNAL_END();
}
} // namespace NLS::Editor::Properties::Tests
NLS_META_EXTERNAL_MODULE(NLS::Editor::Properties::Tests::RegisterCollectionRecord)

namespace
{
using namespace NLS;
using namespace NLS::Editor;
using namespace NLS::Editor::Properties;
using namespace NLS::UI::Controls;
struct Arrays
{
    std::shared_ptr<int> lifetime = std::make_shared<int>();
    std::shared_ptr<Inspector::ReflectedValueBackend> backend;
    std::unique_ptr<Serialization::SerializedObject> object;
    Inspector::PropertyNode node;
    CollectionClipboard clipboard;
    explicit Arrays(std::vector<meta::Variant> values)
    {
        Serialization::SerializedTargetSet targets;
        for (size_t i = 0; i < values.size(); ++i)
            targets.targets.push_back(Inspector::InspectorTargetHandle::FromAsset(Inspector::InspectorTargetKind::AssetImporter,
                                                                                  "collection-test/" + std::to_string(i),
                                                                                  std::to_string(i)));
        backend = std::make_shared<Inspector::ReflectedValueBackend>(std::move(values), lifetime);
        object = std::make_unique<Serialization::SerializedObject>(targets, backend);
        auto nodes = Inspector::InspectorProjectionBuilder::BuildPropertyProjection(*object, targets, {});
        if (!nodes.empty())
            node = nodes.front();
    }
    auto Array() { return object->FindProperty("Value"); }
    auto Run(CollectionCommand command) { return ExecuteCollectionCommand(*object, node, command, clipboard); }
    template<class T>
    std::vector<T> Values(size_t i)
    {
        return backend->Values()[i].GetValue<std::vector<T>>();
    }
};
} // namespace

TEST(CollectionProperties, MixedLengthMovesKeepEveryTargetsOwnValuesAndTails)
{
    Arrays f({meta::Variant(std::vector<int>{1, 2, 3}), meta::Variant(std::vector<int>{10, 20, 30, 40, 50})});
    ASSERT_TRUE(f.Array());
    EXPECT_EQ(f.Array().GetArraySizes(), (std::vector<size_t>{3, 5}));
    CollectionCommand move{CollectionAction::Move, {0}};
    move.insertion = 3;
    ASSERT_TRUE(f.Run(move).changed);
    ASSERT_TRUE(f.Array().GetArrayElementAtIndex(1).SetLongValue(99));
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    EXPECT_EQ(f.Values<int>(0), (std::vector<int>{2, 99, 1}));
    EXPECT_EQ(f.Values<int>(1), (std::vector<int>{20, 99, 10, 40, 50}));
}
TEST(CollectionProperties, MixedResizeDuplicatesEachTargetsOwnLastValue)
{
    Arrays f({meta::Variant(std::vector<int>{1, 2}), meta::Variant(std::vector<int>{10, 20, 30})});
    CollectionCommand resize{CollectionAction::Resize};
    resize.size = 4;
    ASSERT_TRUE(f.Run(resize).changed);
    EXPECT_EQ(f.Array().GetArraySizes(), (std::vector<size_t>{4, 4}));
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    EXPECT_EQ(f.Values<int>(0), (std::vector<int>{1, 2, 2, 2}));
    EXPECT_EQ(f.Values<int>(1), (std::vector<int>{10, 20, 30, 30}));
}
TEST(CollectionProperties, FooterAddUsesSharedVisibleCountAndSelectsNewLastRow)
{
    Arrays f({meta::Variant(std::vector<int>{1, 2}), meta::Variant(std::vector<int>{10, 20, 30, 40})});
    const auto r = f.Run({CollectionAction::Add});
    ASSERT_TRUE(r.changed);
    EXPECT_EQ(r.selection, (std::vector<size_t>{2}));
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    EXPECT_EQ(f.Values<int>(0), (std::vector<int>{1, 2, 2}));
    EXPECT_EQ(f.Values<int>(1), (std::vector<int>{10, 20, 30}));
}
TEST(CollectionProperties, MultiDuplicateAndDeletePreserveTargetDifferences)
{
    Arrays f({meta::Variant(std::vector<int>{1, 2, 3, 4}), meta::Variant(std::vector<int>{10, 20, 30, 40, 50})});
    ASSERT_TRUE(f.Run({CollectionAction::Duplicate, {0, 2}}).changed);
    EXPECT_EQ(f.Array().GetArraySizes(), (std::vector<size_t>{6, 7}));
    ASSERT_TRUE(f.Run({CollectionAction::Remove, {0, 5}}).changed);
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    EXPECT_EQ(f.Values<int>(0), (std::vector<int>{1, 2, 3, 3}));
    EXPECT_EQ(f.Values<int>(1), (std::vector<int>{10, 20, 30, 30, 50}));
}
TEST(CollectionProperties, AppendingBatchUsesEachTargetsEndAndRejectsInvalidTypesAtomically)
{
    Arrays f({meta::Variant(std::vector<int>{1}), meta::Variant(std::vector<int>{10, 20})});
    const meta::Variant invalid[]{std::string("bad")};
    EXPECT_FALSE(f.Array().AppendArrayValues(invalid));
    EXPECT_FALSE(f.object->HasModifiedProperties());
    const meta::Variant values[]{int(7), int(8)};
    ASSERT_TRUE(f.Array().AppendArrayValues(values));
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    EXPECT_EQ(f.Values<int>(0), (std::vector<int>{1, 7, 8}));
    EXPECT_EQ(f.Values<int>(1), (std::vector<int>{10, 20, 7, 8}));
}
TEST(CollectionProperties, StringAlignmentSurvivesReorderResizeAndFurtherEdits)
{
    Arrays f({meta::Variant(std::vector<std::string>{"a", "a very long string", "xyz"}),
              meta::Variant(std::vector<std::string>{"different", "b", "trailing"})});
    CollectionCommand move{CollectionAction::Move, {0}};
    move.insertion = 3;
    ASSERT_TRUE(f.Run(move).changed);
    ASSERT_TRUE(f.Array().GetArrayElementAtIndex(0).SetStringValue("replacement of a different length"));
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    EXPECT_EQ(f.Values<std::string>(0), (std::vector<std::string>{"replacement of a different length", "xyz", "a"}));
    EXPECT_EQ(f.Values<std::string>(1), (std::vector<std::string>{"replacement of a different length", "trailing", "different"}));
}
TEST(CollectionProperties, ComponentEditAfterStructuralChangePreservesUneditedComponents)
{
    Arrays f({meta::Variant(std::vector<Maths::Vector3>{{1, 2, 3}, {4, 5, 6}}),
              meta::Variant(std::vector<Maths::Vector3>{{10, 20, 30}, {40, 50, 60}})});
    CollectionCommand move{CollectionAction::Move, {0}};
    move.insertion = 2;
    ASSERT_TRUE(f.Run(move).changed);
    auto element = f.Array().GetArrayElementAtIndex(0);
    ASSERT_TRUE(element.SetValueComponents(Maths::Vector3(99, 5, 6), 1));
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    EXPECT_EQ(f.Values<Maths::Vector3>(0)[0], Maths::Vector3(99, 5, 6));
    EXPECT_EQ(f.Values<Maths::Vector3>(1)[0], Maths::Vector3(99, 50, 60));
}
TEST(CollectionProperties, SelectedClipboardOwnsLongValuesAndPastesMatchingRows)
{
    Arrays f({meta::Variant(std::vector<std::string>{std::string(9000, 'a'), "second", "third", "fourth"})});
    f.Run({CollectionAction::Copy, {0, 1}});
    ASSERT_TRUE(f.Run({CollectionAction::Paste, {2, 3}}).changed);
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    const auto values = f.Values<std::string>(0);
    EXPECT_EQ(values[2], values[0]);
    EXPECT_EQ(values[3], values[1]);
}
TEST(CollectionProperties, SingleTargetCanExceed1024AndFormatOverflowIsRejected)
{
    Arrays f({meta::Variant(std::vector<int>{1})});
    CollectionCommand resize{CollectionAction::Resize};
    resize.size = 1500;
    ASSERT_TRUE(f.Run(resize).changed);
    EXPECT_EQ(f.Array().GetArraySize(), 1500u);
    EXPECT_FALSE(f.Array().SetArraySize(static_cast<size_t>(INT32_MAX) + 1));
    EXPECT_EQ(f.Array().GetArraySize(), 1500u);
}
TEST(CollectionProperties, ArrayAttributesApplyToElementsAndKeepDeclaringOwner)
{
    EnsureBuiltinPropertyDrawers();
    Arrays f({meta::Variant(std::vector<float>{1, 2})});
    f.node.metadata.customDrawer = "collection-test-range";
    f.node.metadata.rangeMin = 0;
    f.node.metadata.rangeMax = 10;
    f.node.metadata.header = "Only once";
    f.node.metadata.spaceBefore = 20;
    f.node.ownerTypeName = "Owner";
    auto element = MakeCollectionElementNode(f.node, f.Array().GetArrayElementAtIndex(0), 0);
    EXPECT_EQ(element.ownerTypeName, "Owner");
    EXPECT_EQ(element.metadata.customDrawer, "collection-test-range");
    EXPECT_TRUE(element.metadata.rangeMax);
    EXPECT_TRUE(element.metadata.header.empty());
    EXPECT_EQ(element.metadata.spaceBefore, 0);
    PropertyDrawerRegistration registration;
    registration.scope = DrawerScope::ExplicitMetadata;
    registration.metadataKey = registration.identifier = "collection-test-range";
    registration.draw = [](auto&)
    { return true; };
    auto token = PropertyDrawerRegistry::Get().Register(registration);
    EXPECT_NE(PropertyDrawerRegistry::Get().Resolve(f.node)->identifier, "collection-test-range");
    EXPECT_EQ(PropertyDrawerRegistry::Get().Resolve(element)->identifier, "collection-test-range");
}

TEST(CollectionProperties, ReorderCommitsOneUndoAndRestoresBothTargets)
{
    Undo::EditorUndoService undo;
    NLS::Core::ServiceLocator::Provide(undo);
    struct RemoveService
    {
        ~RemoveService() { NLS::Core::ServiceLocator::Remove<Undo::EditorUndoService>(); }
    } cleanup;
    Arrays f({meta::Variant(std::vector<int>{1, 2, 3}), meta::Variant(std::vector<int>{10, 20, 30, 40})});
    CollectionCommand move{CollectionAction::Move, {0}};
    move.insertion = 3;
    ASSERT_TRUE(f.Run(move).changed);
    EXPECT_EQ(undo.GetTransactionCount(), 0u);
    ASSERT_TRUE(f.object->ApplyModifiedProperties()) << f.object->GetLastError();
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_EQ(f.Values<int>(0), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(f.Values<int>(1), (std::vector<int>{10, 20, 30, 40}));
    EXPECT_FALSE(undo.CanUndo());
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    EXPECT_EQ(f.Values<int>(0), (std::vector<int>{2, 3, 1}));
    EXPECT_EQ(f.Values<int>(1), (std::vector<int>{20, 30, 10, 40}));
}

TEST(CollectionProperties, CancelAndExternalRevisionInvalidateUncommittedSessions)
{
    Arrays f({meta::Variant(std::vector<int>{1, 2, 3})});
    auto before = f.Array().GetArrayElementAtIndex(0);
    ASSERT_TRUE(f.Run({CollectionAction::Duplicate, {0}}).changed);
    EXPECT_FALSE(before);
    f.object->DiscardModifiedProperties();
    EXPECT_EQ(f.Array().GetArraySize(), 3u);
    EXPECT_EQ(f.Array().GetArrayElementAtIndex(0).GetLongValue(), 1);
    auto edit = f.object->BeginEditSession("Value.Array.data[0]");
    f.backend->SetSource(meta::Variant(std::vector<int>{70, 80}));
    EXPECT_TRUE(f.object->UpdateIfRequiredOrScript());
    EXPECT_FALSE(edit.Preview(meta::Variant(int(100))));
    EXPECT_FALSE(edit.Commit());
    EXPECT_EQ(f.Array().GetArrayElementAtIndex(0).GetLongValue(), 70);
}

TEST(CollectionProperties, RealPanelSpansColumnsAndRoutesArrayAttributesToVisibleElements)
{
    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    struct Cleanup
    {
        ImGuiContext *gui, *previous;
        ~Cleanup()
        {
            ImGui::DestroyContext(gui);
            ImGui::SetCurrentContext(previous);
        }
    } cleanup{gui, previous};
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000, 800};
    io.DeltaTime = 1.f / 60;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    Arrays f({meta::Variant(std::vector<float>{1, 2, 3})});
    f.Array().SetExpanded(true);
    f.node.metadata.customDrawer = "collection-live-elements";
    float minimumWidth = 10000;
    size_t calls = 0;
    PropertyDrawerRegistration r;
    r.scope = DrawerScope::ExplicitMetadata;
    r.metadataKey = r.identifier = "collection-live-elements";
    r.draw = [&](PropertyDrawerContext& c)
    {
        ++calls;
        EXPECT_EQ(c.property.kind, Inspector::InspectorPropertyKind::Float);
        minimumWidth = (std::min)(minimumWidth, ImGui::GetContentRegionAvail().x);
        ImGui::Dummy({ImGui::GetContentRegionAvail().x, 18});
        return true;
    };
    auto token = PropertyDrawerRegistry::Get().Register(r);
    PropertyPanel panel;
    Inspector::PropertyEditorContext context;
    context.object = f.object.get();
    context.uiScale = 1;
    for (int frame = 0; frame < 2; ++frame)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({900, 700});
        ImGui::Begin("Array panel");
        panel.DrawProperties({f.node}, context);
        ImGui::End();
        ImGui::Render();
    }
    EXPECT_EQ(calls, 6u);
    EXPECT_GT(minimumWidth, 600.f);
}

TEST(CollectionProperties, EditSessionStopsBeforeOwnerDestructionOrExternalReloadCanWrite)
{
    Serialization::SerializedObject invalid;
    auto inactive = invalid.BeginEditSession("Missing");
    EXPECT_FALSE(inactive.IsActive());
    EXPECT_FALSE(inactive.Preview(meta::Variant(int(1))));
    EXPECT_FALSE(inactive.Commit());
    EXPECT_FALSE(inactive.Cancel());
    Arrays f({meta::Variant(std::vector<int>{1, 2, 3})});
    auto edit = f.object->BeginEditSession("Value.Array.data[0]");
    ASSERT_TRUE(edit.Preview(meta::Variant(int(2))));
    f.backend->SetSource(meta::Variant(std::vector<int>{50, 60, 70}));
    EXPECT_FALSE(edit.IsActive());
    EXPECT_FALSE(edit.Preview(meta::Variant(int(100))));
    EXPECT_FALSE(edit.Commit());
    f.object->Update();
    auto destroyed = f.object->BeginEditSession("Value.Array.data[0]");
    f.object.reset();
    EXPECT_FALSE(destroyed.IsActive());
    EXPECT_FALSE(destroyed.Commit());
    EXPECT_FALSE(destroyed.Cancel());
    EXPECT_EQ(f.Values<int>(0), (std::vector<int>{50, 60, 70}));
}

TEST(CollectionProperties, WholeClipboardRoundTripsArrayTypesAndInvalidatesOldElements)
{
    Arrays f({meta::Variant(std::vector<std::string>{"alpha", "beta"})});
    f.Run({CollectionAction::CopyCollection});
    ASSERT_TRUE(f.clipboard.value);
    CollectionCommand resize{CollectionAction::Resize};
    resize.size = 1;
    ASSERT_TRUE(f.Run(resize).changed);
    auto old = f.Array().GetArrayElementAtIndex(0);
    ASSERT_TRUE(f.Run({CollectionAction::PasteCollection}).changed);
    EXPECT_FALSE(old);
    EXPECT_EQ(f.Array().GetArraySize(), 2u);
    EXPECT_EQ(f.Array().GetArrayElementAtIndex(1).GetStringValue(), "beta");
    // Returning to the exact original content does not create an empty edit.
    EXPECT_FALSE(f.object->ApplyModifiedPropertiesWithoutUndo());
    EXPECT_EQ(f.Values<std::string>(0), (std::vector<std::string>{"alpha", "beta"}));
}

TEST(CollectionProperties, NestedArraysRemainIndependentAfterReorderingTheirRecords)
{
    using Record = NLS::Editor::Properties::Tests::CollectionRecord;
    Arrays f({meta::Variant(std::vector<Record>{{1, "first", {1, 2}}, {2, "second", {3}}}),
              meta::Variant(std::vector<Record>{{10, "other first", {100, 200}}, {20, "other second", {300, 400}}})});
    ASSERT_TRUE(f.object->IsValid()) << f.object->GetLastError();
    CollectionCommand move{CollectionAction::Move, {0}};
    move.insertion = 2;
    ASSERT_TRUE(f.Run(move).changed);
    const auto record = f.Array().GetArrayElementAtIndex(0);
    auto children = record.FindPropertyRelative("children");
    ASSERT_TRUE(children);
    ASSERT_TRUE(children.IsArray());
    const meta::Variant added[]{int(7)};
    ASSERT_TRUE(children.AppendArrayValues(added));
    ASSERT_TRUE(f.Array().GetArrayElementAtIndex(0).FindPropertyRelative("title").SetStringValue("Edited"));
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    const auto first = f.Values<Record>(0), second = f.Values<Record>(1);
    EXPECT_EQ(first[0].children, (std::vector<int>{3, 7}));
    EXPECT_EQ(second[0].children, (std::vector<int>{300, 400, 7}));
    EXPECT_EQ(first[0].title, "Edited");
    EXPECT_EQ(second[0].title, "Edited");
    EXPECT_EQ(first[1].number, 1);
    EXPECT_EQ(second[1].number, 10);
}

TEST(CollectionProperties, TwoPanelsKeepFoldoutsSeparateForTheSameSerializedObject)
{
    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    struct Cleanup
    {
        ImGuiContext *gui, *previous;
        ~Cleanup()
        {
            ImGui::DestroyContext(gui);
            ImGui::SetCurrentContext(previous);
        }
    } cleanup{gui, previous};
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000, 800};
    io.DeltaTime = 1.f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    using Record = NLS::Editor::Properties::Tests::CollectionRecord;
    Arrays f({meta::Variant(std::vector<Record>{{1, "One", {1, 2}}, {2, "Two", {3}}, {3, "Three", {4}}})});
    f.Array().GetArrayElementAtIndex(0).SetExpanded(true);
    f.Array().GetArrayElementAtIndex(0).FindPropertyRelative("children").SetExpanded(true);
    PropertyPanel a, b;
    Inspector::PropertyEditorContext context;
    context.object = f.object.get();
    context.uiScale = 1;
    ImVec2 firstMin{};
    float firstHeight = 0, secondHeight = 0;
    const auto frame = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({900, 700});
        ImGui::Begin("Two property panels");
        a.DrawProperties({f.node}, context);
        firstMin = ImGui::GetItemRectMin();
        firstHeight = ImGui::GetItemRectSize().y;
        b.DrawProperties({f.node}, context);
        secondHeight = ImGui::GetItemRectSize().y;
        ImGui::End();
        ImGui::Render();
    };
    frame();
    frame();
    io.AddMousePosEvent(firstMin.x + 10, firstMin.y + 10);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    EXPECT_GT(firstHeight, 180.f);
    EXPECT_LT(secondHeight, 30.f);
    EXPECT_FALSE(f.Array().IsExpanded());
}

TEST(CollectionProperties, UnalignedStringArraysPreservePaddingAndFollowing64BitFields)
{
    using Record = NLS::Editor::Properties::Tests::UnalignedCollectionRecord;
    Arrays f({meta::Variant(Record{true, {"a", "longer", "xyz"}, UINT64_MAX - 5}),
              meta::Variant(Record{false, {"different", "b", "trailing"}, 17})});
    auto array = f.object->FindProperty("Value.items");
    ASSERT_TRUE(array);
    ASSERT_TRUE(array.IsArray());
    const size_t order[]{2, 0, 1};
    ASSERT_TRUE(array.SetArrayOrder(order));
    ASSERT_TRUE(array.SetArraySize(5));
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    auto first = f.backend->Values()[0].GetValue<Record>();
    auto second = f.backend->Values()[1].GetValue<Record>();
    EXPECT_TRUE(first.marker);
    EXPECT_FALSE(second.marker);
    EXPECT_EQ(first.items, (std::vector<std::string>{"xyz", "a", "longer", "longer", "longer"}));
    EXPECT_EQ(second.items, (std::vector<std::string>{"trailing", "different", "b", "b", "b"}));
    EXPECT_EQ(first.tail, UINT64_MAX - 5);
    EXPECT_EQ(second.tail, 17);
    array = f.object->FindProperty("Value.items");
    ASSERT_TRUE(array.ClearArray());
    ASSERT_TRUE(array.SetArraySize(2));
    ASSERT_TRUE(f.object->ApplyModifiedPropertiesWithoutUndo()) << f.object->GetLastError();
    EXPECT_EQ(f.backend->Values()[0].GetValue<Record>().items, (std::vector<std::string>{"", ""}));
    EXPECT_EQ(f.backend->Values()[0].GetValue<Record>().tail, UINT64_MAX - 5);
}

TEST(CollectionProperties, NativeMeshRendererMaterialsReadAndAddThroughTheComponentBackend)
{
    Engine::GameObject object("Native material slots");
    auto* renderer = object.AddComponent<Engine::Components::MeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    using Reference = Engine::Serialize::PPtr<Render::Resources::Material>;
    renderer->SetMaterialReferences(NLS::Array<Reference>{Reference{}, Reference{}});
    ASSERT_EQ(renderer->GetMaterialReferences().size(), 2u);
    Serialization::SerializedTargetSet targets;
    targets.targets = {Inspector::InspectorTargetHandle::FromObject(*renderer, Inspector::InspectorTargetKind::NativeObject)};
    Serialization::SerializedObject serialized(targets, Serialization::NativeReflectionSerializedBackend::Create());
    ASSERT_TRUE(serialized.IsValid()) << serialized.GetLastError();
    auto array = serialized.FindProperty("root.materials");
    ASSERT_TRUE(array) << serialized.GetLastError();
    EXPECT_EQ(array.GetArraySize(), 2u);
    EXPECT_EQ(array.GetArraySizes(), (std::vector<size_t>{2}));
    ASSERT_TRUE(array.SetArraySize(3));
    ASSERT_TRUE(serialized.ApplyModifiedPropertiesWithoutUndo()) << serialized.GetLastError();
    EXPECT_EQ(renderer->GetMaterialReferences().size(), 3u);
    serialized.Update();
    EXPECT_EQ(serialized.FindProperty("root.materials").GetArraySize(), 3u);
}

TEST(CollectionProperties, NativeInspectorParentFieldResolvesItsArrayChildAndFooterAddsARealSlot)
{
    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    struct Cleanup
    {
        ImGuiContext *gui, *previous;
        ~Cleanup()
        {
            ImGui::DestroyContext(gui);
            ImGui::SetCurrentContext(previous);
        }
    } cleanup{gui, previous};
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1400, 1400};
    io.DeltaTime = 1.f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    Undo::EditorUndoService undo;
    NLS::Core::ServiceLocator::Provide(undo);
    struct RemoveUndoService
    {
        ~RemoveUndoService() { NLS::Core::ServiceLocator::Remove<Undo::EditorUndoService>(); }
    } removeUndo;
    Engine::GameObject object("Native inspector material slots");
    auto* renderer = object.AddComponent<Engine::Components::MeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    using Reference = Engine::Serialize::PPtr<Render::Resources::Material>;
    renderer->SetMaterialReferences(NLS::Array<Reference>{Reference{}, Reference{}});
    class ExpandedNativeEditor final : public Inspector::ComponentEditor
    {
        void OnInspectorGUI() override
        {
            auto root = serializedObject().GetIterator();
            if (root.GetPropertyPath().empty())
                (void)root.NextVisible(true);
            root.SetExpanded(true);
            PropertyField(root);
        }
    };
    auto& registry = Inspector::InspectorRegistry::Get();
    Inspector::ComponentEditorRegistration editorRegistration;
    editorRegistration.inspectedType = renderer->GetType();
    editorRegistration.identifier = "CollectionProperties.NativeMaterialInspector";
    editorRegistration.supportsMultiObject = true;
    editorRegistration.priority = 100;
    editorRegistration.create = []
    { return std::make_unique<ExpandedNativeEditor>(); };
    registry.RegisterComponentEditor(std::move(editorRegistration));
    struct Unregister
    {
        ~Unregister() { Inspector::InspectorRegistry::Get().UnregisterComponentEditorsByIdentifierPrefix("CollectionProperties.NativeMaterialInspector"); }
    } unregister;
    size_t draws = 0, observed = 0;
    ImVec2 end{};
    float scale = 1;
    PropertyDrawerRegistration drawer;
    drawer.scope = DrawerScope::ExactType;
    drawer.valueType = NLS_TYPEOF(NLS::Array<Reference>);
    drawer.identifier = "CollectionProperties.NativeMaterialArrayProbe";
    drawer.draw = [&](PropertyDrawerContext& context)
    {
        ++draws;
        EXPECT_EQ(context.property.displayName, "Materials");
        observed = context.serializedProperty ? context.serializedProperty->GetArraySize() : 0;
        EXPECT_TRUE(context.serializedProperty && context.serializedProperty->IsArray());
        if (context.serializedProperty)
            context.serializedProperty->SetExpanded(true);
        const bool drawn = DrawCollectionProperty(context);
        end = ImGui::GetItemRectMax();
        scale = context.uiScale;
        return drawn;
    };
    auto token = PropertyDrawerRegistry::Get().Register(std::move(drawer));
    Inspector::InspectorView view(false);
    view.SetToolbarVisible(false);
    Inspector::InspectorTargetSet targets;
    targets.targets = {Inspector::InspectorTargetHandle::FromObject(object, Inspector::InspectorTargetKind::GameObject)};
    view.SetTargets(targets);
    const auto frame = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1200, 1300});
        ImGui::Begin("Actual native component inspector");
        view.Draw();
        ImGui::End();
        ImGui::Render();
    };
    frame();
    frame();
    ASSERT_GT(draws, 0u);
    ASSERT_EQ(observed, 2u);
    const auto g = GetCollectionGeometry(scale, ImGui::GetFontSize());
    const ImVec2 plus{end.x - 2 * g.footer - g.padding + g.footer * .5f, end.y - g.footer * .5f};
    io.AddMousePosEvent(plus.x, plus.y);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    EXPECT_EQ(renderer->GetMaterialReferences().size(), 3u);
    frame();
    EXPECT_EQ(observed, 3u);
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_EQ(renderer->GetMaterialReferences().size(), 2u);
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    EXPECT_EQ(renderer->GetMaterialReferences().size(), 3u);
}

TEST(CollectionProperties, DefaultNativeInspectorDrawsComponentFieldsWithoutASerializationRootRow)
{
    Engine::GameObject object("Default component fields");
    auto* renderer = object.AddComponent<Engine::Components::MeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    Inspector::PropertyNode component;
    component.target = Inspector::InspectorTargetHandle::FromObject(*renderer, Inspector::InspectorTargetKind::NativeObject);
    Inspector::DefaultComponentEditor editor;
    editor.UpdateSerializedObject(component);
    auto& serialized = editor.GetSerializedObject();
    ASSERT_TRUE(serialized.IsValid()) << serialized.GetLastError();
    std::vector<std::string> fields;
    Inspector::ComponentEditorContextCallbacks callbacks;
    callbacks.drawProperty = [&](const auto& property, const auto&)
    {fields.emplace_back(property.GetPropertyPath());return true; };
    Inspector::ComponentEditorContext context(serialized, serialized.GetTargetSet(), {}, callbacks);
    ASSERT_TRUE(context.DrawDefaultInspector());
    EXPECT_EQ(std::count(fields.begin(), fields.end(), "root"), 0);
    EXPECT_EQ(std::count(fields.begin(), fields.end(), "root.materials"), 1);
    EXPECT_EQ(std::count(fields.begin(), fields.end(), "root.frustumBehaviour"), 1);
    EXPECT_EQ(std::count(fields.begin(), fields.end(), "root.userMatrixValues"), 1);
    EXPECT_EQ(std::count(fields.begin(), fields.end(), "root.materials.Array.size"), 0);
}

TEST(CollectionProperties, NativeExpandedMaterialArrayKeepsThePanelGeometryStableAcrossFrames)
{
    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    struct Cleanup
    {
        ImGuiContext *gui, *previous;
        ~Cleanup()
        {
            ImGui::DestroyContext(gui);
            ImGui::SetCurrentContext(previous);
        }
    } cleanup{gui, previous};
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1600, 1400};
    io.DeltaTime = 1.f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    Engine::GameObject object("Stable native inspector");
    auto* renderer = object.AddComponent<Engine::Components::MeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    using Reference = Engine::Serialize::PPtr<Render::Resources::Material>;
    renderer->SetMaterialReferences(NLS::Array<Reference>(std::vector<Reference>(16)));
    size_t draws = 0;
    ImVec2 listMin{}, listMax{};
    bool preopen = true;
    ImGuiID drawHash = 0;

    PropertyDrawerRegistration arrayDrawer;
    arrayDrawer.scope = DrawerScope::ExactType;
    arrayDrawer.valueType = NLS_TYPEOF(NLS::Array<Reference>);
    arrayDrawer.identifier = "CollectionProperties.StableNativeMaterialArray";
    arrayDrawer.draw = [&](PropertyDrawerContext& c)
    {
        ++draws;
        if (preopen)
            c.serializedProperty->SetExpanded(true);
        const bool drawn = DrawCollectionProperty(c);
        listMin = ImGui::GetItemRectMin();
        listMax = ImGui::GetItemRectMax();
        return drawn;
    };
    auto arrayToken = PropertyDrawerRegistry::Get().Register(std::move(arrayDrawer));
    Inspector::InspectorView view(false);
    view.SetToolbarVisible(false);
    Inspector::InspectorTargetSet targets;
    targets.targets = {Inspector::InspectorTargetHandle::FromObject(object, Inspector::InspectorTargetKind::GameObject)};
    view.SetTargets(targets);
    const auto frame = [&](ImVec2 size)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("Stationary native array");
        view.Draw();
        auto* window = ImGui::GetCurrentWindow();
        const std::array<float, 6> metrics{listMin.x, listMin.y, listMax.x, listMax.y, window->DC.CursorMaxPos.y, window->ScrollMax.y};
        ImGui::End();
        ImGui::Render();
        drawHash = 0;
        for (const auto* draw : ImGui::GetDrawData()->CmdLists)
        {
            drawHash = ImHashData(draw->VtxBuffer.Data, draw->VtxBuffer.Size * sizeof(ImDrawVert), drawHash);
            drawHash = ImHashData(draw->IdxBuffer.Data, draw->IdxBuffer.Size * sizeof(ImDrawIdx), drawHash);
            for (const auto& command : draw->CmdBuffer)
                drawHash = ImHashData(&command.ClipRect, sizeof(command.ClipRect), drawHash);
        }
        return metrics;
    };
    for (const ImVec2 size : {ImVec2(450, 420), ImVec2(600, 600), ImVec2(1200, 1100)})
    {
        for (int warmup = 0; warmup < 8; ++warmup)
            frame(size);
        ASSERT_GT(draws, 0u);
        ASSERT_GT(listMax.y - listMin.y, 50.f);
        const auto expected = frame(size);
        const auto expectedHash = drawHash;
        for (int i = 0; i < 24; ++i)
        {
            const auto actual = frame(size);
            EXPECT_EQ(drawHash, expectedHash) << "render width=" << size.x << " frame=" << i;
            for (size_t n = 0; n < actual.size(); ++n)
                EXPECT_NEAR(actual[n], expected[n], .1f) << "width=" << size.x << " frame=" << i << " metric=" << n;
        }
        EXPECT_EQ(renderer->GetMaterialReferences().size(), 16u);
    }
    // Exercise actual foldout toggles, with no forced expanded cursor state.
    preopen = false;
    const ImVec2 size{600, 600};
    for (int warmup = 0; warmup < 8; ++warmup)
        frame(size);
    const ImVec2 foldout{listMin.x + 8, listMin.y + 8};
    for (int toggle = 0; toggle < 2; ++toggle)
    {
        io.AddMousePosEvent(foldout.x, foldout.y);
        io.AddMouseButtonEvent(0, true);
        frame(size);
        io.AddMouseButtonEvent(0, false);
        frame(size);
        io.AddMousePosEvent(-100, -100);
        for (int warmup = 0; warmup < 8; ++warmup)
            frame(size);
        const auto expected = frame(size);
        const auto expectedHash = drawHash;
        for (int i = 0; i < 36; ++i)
        {
            const auto actual = frame(size);
            EXPECT_EQ(drawHash, expectedHash) << "render toggle=" << toggle << " frame=" << i;
            for (size_t n = 0; n < actual.size(); ++n)
                EXPECT_NEAR(actual[n], expected[n], .1f) << "natural toggle=" << toggle << " frame=" << i << " metric=" << n;
        }
    }
}

TEST(CollectionProperties, NativeComponentRefreshPreservesExpansionAndCachedCursors)
{
    Engine::GameObject object("Persistent native component state");
    auto* renderer = object.AddComponent<Engine::Components::MeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    Inspector::PropertyNode component;
    component.target = Inspector::InspectorTargetHandle::FromObject(*renderer, Inspector::InspectorTargetKind::NativeObject);
    Inspector::DefaultComponentEditor editor;
    editor.UpdateSerializedObject(component);
    auto& serialized = editor.GetSerializedObject();
    using Reference = Engine::Serialize::PPtr<Render::Resources::Material>;
    auto array = serialized.FindProperty("root.materials");
    ASSERT_TRUE(array);
    array.SetExpanded(true);
    renderer->SetMaterialReferences(NLS::Array<Reference>{Reference{}, Reference{}});
    for (int i = 0; i < 8; ++i)
    {
        editor.UpdateSerializedObject(component);
        EXPECT_TRUE(array.IsExpanded()) << "frame=" << i;
        EXPECT_TRUE(serialized.FindProperty("root.materials").IsExpanded()) << "frame=" << i;
        EXPECT_EQ(array.GetArraySize(), 2u);
    }
    ASSERT_TRUE(array.SetArraySize(3));
    editor.UpdateSerializedObject(component);
    EXPECT_EQ(array.GetArraySize(), 3u);
    EXPECT_TRUE(array.IsExpanded());
    ASSERT_TRUE(serialized.ApplyModifiedPropertiesWithoutUndo()) << serialized.GetLastError();
    editor.UpdateSerializedObject(component);
    EXPECT_EQ(array.GetArraySize(), 3u);
    EXPECT_TRUE(array.IsExpanded());
    renderer->SetMaterialReferences(NLS::Array<Reference>{Reference{}});
    editor.UpdateSerializedObject(component);
    EXPECT_EQ(array.GetArraySize(), 1u);
    EXPECT_TRUE(array.IsExpanded());
}

TEST(CollectionProperties, BuiltinUserMatrixRowsRemainStableWithTheEditorFont)
{
    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    struct Cleanup
    {
        ImGuiContext *gui, *previous;
        ~Cleanup()
        {
            ImGui::DestroyContext(gui);
            ImGui::SetCurrentContext(previous);
        }
    } cleanup{gui, previous};
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1800, 1400};
    io.DeltaTime = 1.f / 60;
    io.ConfigInputTrickleEventQueue = false;
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {10, 10};
    style.FramePadding = {6, 4};
    style.ItemSpacing = {8, 6};
    style.ItemInnerSpacing = {6, 4};
    style.IndentSpacing = 18;
    style.ScrollbarSize = 12;
    auto* font = io.Fonts->AddFontFromFileTTF("App/Assets/Editor/Fonts/Ruda-Bold.ttf", 14);
    ASSERT_NE(font, nullptr);
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    Engine::GameObject object("Builtin matrix rows");
    auto* renderer = object.AddComponent<Engine::Components::MeshRenderer>();
    ASSERT_NE(renderer, nullptr);
    auto matrixRect = std::make_shared<ImRect>();
    class OpenMatrix final : public Inspector::ComponentEditor
    {
        std::shared_ptr<ImRect> rectangle;

    public:
        explicit OpenMatrix(std::shared_ptr<ImRect> value)
            : rectangle(std::move(value)) {}
        void OnInspectorGUI() override
        {
            EXPECT_EQ(serializedObject().FindProperty("root.userMatrixValues").GetMinArraySize(), 16u);
            PropertyField("root.materials");
            PropertyField("root.userMatrixValues");
            *rectangle = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
    };
    Inspector::ComponentEditorRegistration registration;
    registration.inspectedType = renderer->GetType();
    registration.identifier = "CollectionProperties.BuiltinMatrixStability";
    registration.priority = 100;
    registration.supportsMultiObject = true;
    registration.create = [matrixRect]
    { return std::make_unique<OpenMatrix>(matrixRect); };
    Inspector::InspectorRegistry::Get().RegisterComponentEditor(std::move(registration));
    struct Unregister
    {
        ~Unregister() { Inspector::InspectorRegistry::Get().UnregisterComponentEditorsByIdentifierPrefix("CollectionProperties.BuiltinMatrixStability"); }
    } unregister;
    Inspector::InspectorView view(false);
    view.SetToolbarVisible(false);
    Inspector::InspectorTargetSet targets;
    targets.targets = {Inspector::InspectorTargetHandle::FromObject(object, Inspector::InspectorTargetKind::GameObject)};
    view.SetTargets(targets);
    const auto frame = [&](float width, float height)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({width, height});
        ImGui::Begin("Builtin matrix stability");
        view.Draw();
        const auto* window = ImGui::GetCurrentWindow();
        const std::array<float, 4> result{window->DC.CursorMaxPos.y, window->ScrollMax.y, window->ScrollbarY ? 1.f : 0.f, window->ContentSize.x};
        ImGui::End();
        ImGui::Render();
        return result;
    };
    frame(600, 800);
    frame(600, 800);
    ASSERT_LT(matrixRect->GetHeight(), 40.f);
    io.AddMousePosEvent(matrixRect->Min.x + 8, matrixRect->Min.y + 8);
    io.AddMouseButtonEvent(0, true);
    frame(600, 800);
    io.AddMouseButtonEvent(0, false);
    frame(600, 800);
    io.AddMousePosEvent(-100, -100);
    for (int i = 0; i < 10; ++i)
    {
        frame(600, 800);
        EXPECT_GT(matrixRect->GetHeight(), 100.f) << "open frame=" << i;
    }
    for (int i = 0; i < 10; ++i)
        frame(600, 120);
    auto* scrolled = ImGui::FindWindowByName("Builtin matrix stability");
    ASSERT_NE(scrolled, nullptr);
    const auto contentHeight = scrolled->ContentSize.y;
    ASSERT_GT(scrolled->ScrollMax.y, 10.f);
    scrolled->Scroll.y = (std::min)(150.f, scrolled->ScrollMax.y);
    const auto requestedScroll = scrolled->Scroll.y;
    for (int i = 0; i < 16; ++i)
    {
        frame(600, 120);
        EXPECT_NEAR(scrolled->ContentSize.y, contentHeight, .1f) << "scrolled frame=" << i;
        EXPECT_NEAR(scrolled->Scroll.y, requestedScroll, .1f) << "scroll clamped frame=" << i;
    }
    scrolled->Scroll.y = 0;
    for (float scale : {1.f, 1.5f, 2.f})
    {
        io.FontGlobalScale = scale;
        for (float width : {450.f, 600.f})
            for (float height : {300.f, 450.f, 600.f, 800.f})
            {
                for (int i = 0; i < 8; ++i)
                    frame(width, height);
                auto expected = frame(width, height);
                for (int i = 0; i < 24; ++i)
                {
                    auto actual = frame(width, height);
                    for (size_t j = 0; j < actual.size(); ++j)
                        EXPECT_NEAR(actual[j], expected[j], .1f) << "width=" << width << " height=" << height << " scale=" << scale << " frame=" << i << " metric=" << j;
                }
            }
    }
}

TEST(CollectionProperties, UserMatrixUsesTheMatrixDrawerAndEditsOneCellAcrossTargetsWithOneUndo)
{
    auto* previous = ImGui::GetCurrentContext();
    auto* gui = ImGui::CreateContext();
    struct Cleanup
    {
        ImGuiContext *gui, *previous;
        ~Cleanup()
        {
            ImGui::DestroyContext(gui);
            ImGui::SetCurrentContext(previous);
        }
    } cleanup{gui, previous};
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1200, 900};
    io.DeltaTime = 1.f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    Undo::EditorUndoService undo;
    NLS::Core::ServiceLocator::Provide(undo);
    struct RemoveUndo
    {
        ~RemoveUndo() { NLS::Core::ServiceLocator::Remove<Undo::EditorUndoService>(); }
    } removeUndo;
    Engine::GameObject first("First matrix"), second("Second matrix");
    auto* a = first.AddComponent<Engine::Components::MeshRenderer>();
    auto* b = second.AddComponent<Engine::Components::MeshRenderer>();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    a->SetUserMatrixElement(0, 0, 31);
    b->SetUserMatrixElement(0, 0, 41);
    a->SetUserMatrixElement(3, 3, 1);
    b->SetUserMatrixElement(3, 3, 2);
    Serialization::SerializedTargetSet targets;
    targets.targets = {Inspector::InspectorTargetHandle::FromObject(*a, Inspector::InspectorTargetKind::NativeObject),
                       Inspector::InspectorTargetHandle::FromObject(*b, Inspector::InspectorTargetKind::NativeObject)};
    Serialization::SerializedObject object(targets, Serialization::NativeReflectionSerializedBackend::Create());
    ASSERT_TRUE(object.IsValid()) << object.GetLastError();
    auto projection = Inspector::InspectorProjectionBuilder::BuildPropertyProjection(object, targets, {});
    Inspector::PropertyNode matrix;
    const auto find = [&](auto&& self, const auto& nodes) -> void
    {for(const auto& node:nodes){if(node.path.ToString()=="root.userMatrixValues")matrix=node;self(self,node.children);} };
    find(find, projection);
    ASSERT_EQ(matrix.path.ToString(), "root.userMatrixValues");
    ASSERT_EQ(object.FindProperty("root.userMatrixValues").GetMinArraySize(), 16u);
    PropertyPanel panel;
    auto resolved = PropertyDrawerRegistry::Get().Resolve(matrix);
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->identifier, "builtin.mesh-renderer.user-matrix");
    Arrays ordinary({meta::Variant(std::vector<float>{1, 2, 3})});
    EXPECT_NE(PropertyDrawerRegistry::Get().Resolve(ordinary.node)->identifier, "builtin.mesh-renderer.user-matrix");
    object.FindProperty("root.userMatrixValues").SetExpanded(true);
    Inspector::PropertyEditorContext context;
    context.object = &object;
    context.uiScale = 1;
    ImVec2 end{};
    const auto frame = [&]
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1000, 700});
        ImGui::Begin("Matrix cell edit");
        panel.DrawProperties({matrix}, context);
        end = ImGui::GetItemRectMax();
        ImGui::End();
        ImGui::Render();
    };
    frame();
    frame();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddMousePosEvent(end.x - 45, end.y - ImGui::GetFrameHeight() * .5f);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    io.AddInputCharactersUTF8("7");
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();
    EXPECT_EQ(a->GetUserMatrixElement(3, 3), 7);
    EXPECT_EQ(b->GetUserMatrixElement(3, 3), 7);
    EXPECT_EQ(a->GetUserMatrixElement(0, 0), 31);
    EXPECT_EQ(b->GetUserMatrixElement(0, 0), 41);
    EXPECT_EQ(a->GetUserMatrixValues().size(), 16u);
    EXPECT_EQ(b->GetUserMatrixValues().size(), 16u);
    EXPECT_EQ(undo.GetTransactionCount(), 1u);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_EQ(a->GetUserMatrixElement(3, 3), 1);
    EXPECT_EQ(b->GetUserMatrixElement(3, 3), 2);
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    EXPECT_EQ(a->GetUserMatrixElement(3, 3), 7);
    EXPECT_EQ(b->GetUserMatrixElement(3, 3), 7);
}
