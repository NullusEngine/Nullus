#include <Reflection/Enum.h>
#include <gtest/gtest.h>
#include "Properties/PropertyPanel.h"
#include "Inspector/ReflectedEditHost.h"
#include "Inspector/InspectorView.h"
#include "Properties/PropertyDrawerRegistry.h"
#include "Properties/CurveValue.h"
#include <Serialize/SerializedSpecialTypes.h>
#include <Serialize/ExternalReflection.h>
#include <ServiceLocator.h>
#include <Math/Matrix4.h>

namespace NLS::Editor::Properties::Tests
{
enum class WideFlags : uint64_t
{
    Low = 1,
    High = 0x8000000000000000ull
};
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Properties::Tests::WideFlags)
inline void RegisterWideFlags(meta::ReflectionDatabase& db, meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Properties::Tests::WideFlags)
    metaExternalType.SetEnum<WideFlags>(metaExternalTypeName, {{"Low", WideFlags::Low}, {"High", WideFlags::High}});
    NLS_META_EXTERNAL_END();
}
} // namespace NLS::Editor::Properties::Tests
NLS_META_EXTERNAL_MODULE(NLS::Editor::Properties::Tests::RegisterWideFlags)

namespace
{
using namespace NLS;
using namespace NLS::Editor;
struct PanelGui
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    PanelGui()
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1000, 800};
        io.DeltaTime = 1.f / 60;
        unsigned char* pixels;
        int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~PanelGui()
    {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    void Begin()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({900, 700});
        ImGui::Begin("Unified properties");
    }
    void End()
    {
        ImGui::End();
        ImGui::Render();
    }
};
class ExampleDrawer : public Properties::IPropertyDrawer
{
public:
    int calls = 0;
    UI::Controls::ControlResult Draw(Properties::PropertyDrawerContext& context) override
    {
        ++calls;
        ImGui::TextUnformatted("Example independent control");
        context.value = 42.f;
        return {true, false, true, true, false, 1};
    }
};
Serialization::SerializedTargetSet MultiTargets(size_t count)
{
    Serialization::SerializedTargetSet targets;
    for (size_t i = 0; i < count; ++i)
        targets.targets.push_back(Inspector::InspectorTargetHandle::FromAsset(
            Inspector::InspectorTargetKind::AssetImporter, "unified-test/" + std::to_string(i), std::to_string(i), "ReflectedProperties"));
    return targets;
}
} // namespace
TEST(UnifiedPropertyPanel, RegisteredControlWorksForObjectAndValueThenUnregisters)
{
    PanelGui gui;
    auto drawer = std::make_shared<ExampleDrawer>();
    Properties::PropertyDrawerRegistration registration;
    registration.scope = Properties::DrawerScope::ExplicitMetadata;
    registration.metadataKey = "UnifiedPropertyPanel.Example";
    registration.identifier = registration.metadataKey;
    auto token = Properties::PropertyDrawerRegistry::Get().Register(registration, drawer);
    Properties::PropertyPanel panel;
    Inspector::ReflectedPropertyOptions options;
    options.metadata.emplace();
    options.metadata->customDrawer = registration.metadataKey;
    float value = 1;
    gui.Begin();
    const auto edit = panel.DrawValue("value", "Value", value, options);
    gui.End();
    EXPECT_TRUE(edit.changed);
    EXPECT_TRUE(edit.completed);
    EXPECT_FLOAT_EQ(value, 42);
    const auto lifetime = std::make_shared<int>();
    auto backend = std::make_shared<Inspector::ReflectedValueBackend>(meta::Variant(2.f), lifetime);
    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(
        Inspector::InspectorTargetHandle::FromAsset(Inspector::InspectorTargetKind::AssetImporter, "Unified", "0"));
    Serialization::SerializedObject object(targets, backend);
    auto nodes = Inspector::InspectorProjectionBuilder::BuildPropertyProjection(object, targets, {});
    ASSERT_EQ(nodes.size(), 1u);
    nodes[0].metadata.customDrawer = registration.metadataKey;
    Inspector::PropertyEditorContext context;
    context.object = &object;
    context.apply = [&]
    { return object.ApplyModifiedPropertiesWithoutUndo(); };
    gui.Begin();
    const auto objectEdit = panel.DrawProperties(nodes, context);
    gui.End();
    EXPECT_TRUE(objectEdit.applied);
    EXPECT_FLOAT_EQ(backend->Value().GetValue<float>(), 42);
    EXPECT_EQ(drawer->calls, 2);
    token.Reset();
    value = 9;
    gui.Begin();
    const auto after = panel.DrawValue("value", "Value", value, options);
    gui.End();
    EXPECT_FALSE(after.changed);
    EXPECT_FLOAT_EQ(value, 9);
    EXPECT_EQ(drawer->calls, 2);
}
TEST(UnifiedPropertyPanel, ScopedFieldRegistrationRequiresOwnerAndAllowsDifferentOwners)
{
    Inspector::InspectorRegistry storage;
    Properties::PropertyDrawerRegistry registry(storage);
    Properties::PropertyDrawerRegistration registration;
    registration.scope = Properties::DrawerScope::Field;
    registration.identifier = "field-a";
    registration.fieldPath = Inspector::PropertyPath("Value.amount");
    registration.draw = [](auto&)
    { return true; };
    EXPECT_THROW((void)registry.Register(registration), std::invalid_argument);
    registration.ownerTypeName = "TypeA";
    auto a = registry.Register(registration);
    registration.identifier = "field-b";
    registration.ownerTypeName = "TypeB";
    auto b = registry.Register(registration);
    Inspector::PropertyNode node;
    node.path = registration.fieldPath;
    node.ownerTypeName = "TypeA";
    ASSERT_TRUE(registry.Resolve(node));
    EXPECT_EQ(registry.Resolve(node)->identifier, "field-a");
    node.ownerTypeName = "TypeB";
    EXPECT_EQ(registry.Resolve(node)->identifier, "field-b");
    EXPECT_THROW((void)registry.Register(registration), std::logic_error);
    b.Reset();
    EXPECT_FALSE(registry.Resolve(node));
}
TEST(UnifiedPropertyPanel, AdvancedValuesDrawWithoutChangingSerializedContent)
{
    PanelGui gui;
    Properties::PropertyPanel panel;
    Engine::Serialize::AnimationCurve curve;
    curve.keys = {{0, 2, 3, 4, .2f, .4f, 3}, {1, 7, 8, 9, .3f, .6f, 1}};
    curve.preWrapMode = 3;
    curve.postWrapMode = 4;
    Engine::Serialize::Gradient gradient;
    gradient.colorKeys = {{Maths::Color(2, .3f, .5f, .7f), 0}, {Maths::Color(1, 2, 3, .9f), 1}};
    gradient.alphaKeys = {{.4f, .2f}, {.8f, .9f}};
    const auto originalCurve = curve;
    const auto originalGradient = gradient;
    gui.Begin();
    EXPECT_FALSE(panel.DrawValue("curve", "Curve", curve).changed);
    EXPECT_FALSE(panel.DrawValue("gradient", "Gradient", gradient).changed);
    gui.End();
    EXPECT_EQ(curve, originalCurve);
    EXPECT_EQ(gradient, originalGradient);
}

TEST(UnifiedPropertyPanel, ComponentEditsPreserveEverySecondaryTargetField)
{
    const auto lifetime = std::make_shared<int>();
    auto backend = std::make_shared<Inspector::ReflectedValueBackend>(
        std::vector<meta::Variant>{Maths::Vector3(1, 2, 3), Maths::Vector3(4, 5, 6)}, lifetime);
    Serialization::SerializedObject object(MultiTargets(2), backend);
    auto property = object.FindProperty("Value");
    ASSERT_TRUE(property.SetValueComponents(meta::Variant(Maths::Vector3(9, 2, 3)), 1));
    auto staged = property.GetTargetValues();
    ASSERT_EQ(staged.size(), 2u);
    EXPECT_FLOAT_EQ(staged[1].GetValue<Maths::Vector3>().x, 9);
    EXPECT_FLOAT_EQ(staged[1].GetValue<Maths::Vector3>().y, 5);
    EXPECT_TRUE(property.HasMultipleDifferentValues());
    ASSERT_TRUE(object.ApplyModifiedPropertiesWithoutUndo());
    EXPECT_FLOAT_EQ(backend->Values()[1].GetValue<Maths::Vector3>().z, 6);
}
TEST(UnifiedPropertyPanel, QuaternionAxisEditPreservesTheOtherTargetsEulerAxes)
{
    const auto lifetime = std::make_shared<int>();
    const Maths::Quaternion first(Maths::Vector3(5, 10, 15)), second(Maths::Vector3(20, 25, 30));
    const auto secondEuler = Maths::Quaternion::EulerAngles(second);
    auto backend = std::make_shared<Inspector::ReflectedValueBackend>(std::vector<meta::Variant>{first, second}, lifetime);
    Serialization::SerializedObject object(MultiTargets(2), backend);
    auto edited = Maths::Quaternion::EulerAngles(first);
    edited.x = 35;
    ASSERT_TRUE(object.FindProperty("Value").SetValueComponents(meta::Variant(Maths::Quaternion(edited)), 1));
    ASSERT_TRUE(object.ApplyModifiedPropertiesWithoutUndo());
    const auto result = Maths::Quaternion::EulerAngles(backend->Values()[1].GetValue<Maths::Quaternion>());
    EXPECT_NEAR(result.x, 35, 1e-3f);
    EXPECT_NEAR(result.y, secondEuler.y, 1e-3f);
    EXPECT_NEAR(result.z, secondEuler.z, 1e-3f);
}
TEST(UnifiedPropertyPanel, ComponentGestureRecordsOneUndoWithPerTargetSnapshots)
{
    Undo::EditorUndoService service;
    auto* previous = NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>() ? &NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>() : nullptr;
    NLS::Core::ServiceLocator::Provide(service);
    struct Restore
    {
        Undo::EditorUndoService* previous;
        ~Restore()
        {
            NLS::Core::ServiceLocator::Remove<Undo::EditorUndoService>();
            if (previous)
                NLS::Core::ServiceLocator::Provide(*previous);
        }
    } restore{previous};
    const auto lifetime = std::make_shared<int>();
    auto backend = std::make_shared<Inspector::ReflectedValueBackend>(
        std::vector<meta::Variant>{Maths::Vector3(1, 2, 3), Maths::Vector3(4, 5, 6)}, lifetime);
    Serialization::SerializedObject object(MultiTargets(2), backend);
    for (float x : {7.f, 8.f, 9.f})
        ASSERT_TRUE(object.FindProperty("Value").SetValueComponents(meta::Variant(Maths::Vector3(x, 2, 3)), 1));
    ASSERT_TRUE(object.ApplyModifiedProperties());
    EXPECT_EQ(service.GetHistoryState().transactionCount, 1u);
    ASSERT_TRUE(service.RequestUndo().Succeeded());
    EXPECT_FLOAT_EQ(backend->Values()[0].GetValue<Maths::Vector3>().x, 1);
    EXPECT_FLOAT_EQ(backend->Values()[1].GetValue<Maths::Vector3>().x, 4);
    ASSERT_TRUE(service.RequestRedo().Succeeded());
    EXPECT_FLOAT_EQ(backend->Values()[1].GetValue<Maths::Vector3>().x, 9);
    EXPECT_FLOAT_EQ(backend->Values()[1].GetValue<Maths::Vector3>().y, 5);
}
TEST(UnifiedPropertyPanel, MatrixReflectionPreservesAllSixteenValues)
{
    const auto lifetime = std::make_shared<int>();
    Maths::Matrix4 matrix;
    for (size_t i = 0; i < 16; ++i)
        matrix.data[i] = static_cast<float>(i) + .25f;
    auto backend = std::make_shared<Inspector::ReflectedValueBackend>(meta::Variant(matrix), lifetime);
    Serialization::SerializedObject object(MultiTargets(1), backend);
    const auto copy = object.FindProperty("Value").GetValue();
    ASSERT_TRUE(copy.IsValid());
    for (size_t i = 0; i < 16; ++i)
        EXPECT_FLOAT_EQ(copy.GetValue<Maths::Matrix4>().data[i], matrix.data[i]);
}
TEST(UnifiedPropertyPanel, DrawValuesReportsConflictingSecondaryChanges)
{
    PanelGui gui;
    Properties::PropertyPanel panel;
    Properties::PropertyDrawerRegistration registration;
    registration.scope = Properties::DrawerScope::ExplicitMetadata;
    registration.identifier = "UnifiedPropertyPanel.conflict";
    registration.metadataKey = registration.identifier;
    registration.draw = [](auto& context)
    {
        ImGui::TextUnformatted("Continuous preview");
        context.interaction = {true, true, true, false, false, 1};
        context.changed = context.preview(meta::Variant(9.f), "Preview");
        return true;
    };
    auto token = Properties::PropertyDrawerRegistry::Get().Register(registration);
    Inspector::ReflectedPropertyOptions options;
    options.metadata.emplace();
    options.metadata->customDrawer = registration.metadataKey;
    std::vector<float> values{1, 2};
    gui.Begin();
    const auto first = panel.DrawValues("conflict", "Values", values, options);
    gui.End();
    ASSERT_TRUE(first.changed);
    values[1] = 17;
    gui.Begin();
    const auto second = panel.DrawValues("conflict", "Values", values, options);
    gui.End();
    EXPECT_TRUE(second.canceled);
    EXPECT_FALSE(second.changed);
    EXPECT_FLOAT_EQ(values[1], 17);
}

namespace
{
struct ExtensionUiState
{
    static inline int live = 0;
    int draws = 0;
    ExtensionUiState() { ++live; }
    ~ExtensionUiState() { --live; }
};
} // namespace
TEST(UnifiedPropertyPanel, ModuleUnregisterDestroysTypedUiStateImmediately)
{
    PanelGui gui;
    Properties::PropertyPanel first, second;
    Properties::PropertyDrawerRegistration registration;
    registration.scope = Properties::DrawerScope::ExplicitMetadata;
    registration.identifier = registration.metadataKey = "UnifiedPropertyPanel.typed-state";
    registration.draw = [](Properties::PropertyDrawerContext& context)
    {auto& state=context.GetState<ExtensionUiState>();++state.draws;ImGui::Text("State %d",state.draws);return true; };
    auto token = Properties::PropertyDrawerRegistry::Get().Register(registration);
    Inspector::ReflectedPropertyOptions options;
    options.metadata.emplace();
    options.metadata->customDrawer = registration.metadataKey;
    float a = 1, b = 2;
    gui.Begin();
    first.DrawValue("same-id", "First", a, options);
    second.DrawValue("same-id", "Second", b, options);
    gui.End();
    EXPECT_EQ(ExtensionUiState::live, 2);
    token.Reset();
    EXPECT_EQ(ExtensionUiState::live, 0);
    EXPECT_FLOAT_EQ(a, 1);
    EXPECT_FLOAT_EQ(b, 2);
}

TEST(UnifiedPropertyPanel, SelectionChangesDuringDrawAreDeferredUntilTheNextFrame)
{
    PanelGui gui;
    Engine::GameObject first("First"), second("Second");
    Inspector::InspectorView view(false);
    Inspector::InspectorTargetSet a, b;
    a.targets.push_back(Inspector::InspectorTargetHandle::FromObject(first, Inspector::InspectorTargetKind::GameObject));
    b.targets.push_back(Inspector::InspectorTargetHandle::FromObject(second, Inspector::InspectorTargetKind::GameObject));
    view.SetTargets(a);
    bool switched = false;
    Properties::PropertyDrawerRegistration registration;
    registration.scope = Properties::DrawerScope::ExactType;
    registration.identifier = "UnifiedPropertyPanel.selection-change";
    registration.valueType = NLS_TYPEOF(Maths::Vector3);
    registration.draw = [&](auto&)
    {ImGui::TextUnformatted("Selection callback");if(!switched) {switched=true;view.SetTargets(b);}return true; };
    auto token = Properties::PropertyDrawerRegistry::Get().Register(registration);
    gui.Begin();
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    view.Draw();
    gui.End();
    ASSERT_TRUE(switched);
    EXPECT_EQ(view.GetTargets().targets.front().instanceId, a.targets.front().instanceId);
    gui.Begin();
    view.Draw();
    gui.End();
    EXPECT_EQ(view.GetTargets().targets.front().instanceId, b.targets.front().instanceId);
}

TEST(UnifiedPropertyPanel, UnregisterCancelsTheOpenDocumentGesture)
{
    PanelGui gui;
    Properties::PropertyPanel panel;
    struct Host : Inspector::IReflectedEditHost
    {
        float live = 1, candidate = 9, before = 1;
        bool open = false;
        int commits = 0, cancels = 0;
        bool BeginPropertyEdit(std::string_view) override
        {
            before = live;
            open = true;
            return true;
        }
        bool PreviewPropertyEdit() override
        {
            live = candidate;
            return true;
        }
        bool CommitPropertyEdit() override
        {
            open = false;
            ++commits;
            return true;
        }
        bool CancelPropertyEdit() override
        {
            live = before;
            open = false;
            ++cancels;
            return true;
        }
        bool IsPropertyEditOpen() const override { return open; }
    } host;
    Properties::PropertyDrawerRegistration registration;
    registration.scope = Properties::DrawerScope::ExplicitMetadata;
    registration.identifier = registration.metadataKey = "UnifiedPropertyPanel.unregister-gesture";
    registration.draw = [](Properties::PropertyDrawerContext& context)
    {ImGui::TextUnformatted("Pending control");context.interaction={true,true,true,false,false,1};context.changed=context.preview(meta::Variant(9.f),"Pending");return true; };
    auto token = Properties::PropertyDrawerRegistry::Get().Register(registration);
    Inspector::ReflectedPropertyOptions options;
    options.metadata.emplace();
    options.metadata->customDrawer = registration.metadataKey;
    float value = 1;
    gui.Begin();
    const auto edit = panel.DrawValue("pending", "Value", value, options);
    ASSERT_TRUE(Inspector::ApplyReflectedEdit(host, edit, "Pending"));
    EXPECT_FLOAT_EQ(host.live, 9);
    token.Reset();
    (void)Inspector::FinishReflectedEdits(panel, host);
    gui.End();
    EXPECT_EQ(host.cancels, 1);
    EXPECT_EQ(host.commits, 0);
    EXPECT_FLOAT_EQ(host.live, 1);
}

TEST(UnifiedPropertyPanel, WideFlagCombinationsPreserveAllBitsAndUseTheFlagsDrawer)
{
    PanelGui gui;
    Properties::PropertyPanel panel;
    const auto type = NLS_TYPEOF(Properties::Tests::WideFlags);
    const uint64_t bits = 0x8000000000000001ull;
    const auto lifetime = std::make_shared<int>();
    auto backend = std::make_shared<Inspector::ReflectedValueBackend>(meta::Variant(Properties::Tests::WideFlags::Low), lifetime);
    Serialization::SerializedObject object(MultiTargets(1), backend);
    ASSERT_TRUE(object.FindProperty("Value").SetValue(type.GetEnum().FromBits(bits)));
    ASSERT_TRUE(object.ApplyModifiedPropertiesWithoutUndo());
    EXPECT_EQ(type.GetEnum().GetBits(backend->Value()), bits);
    Properties::PropertyDrawerRegistration registration;
    registration.scope = Properties::DrawerScope::ExplicitMetadata;
    registration.identifier = registration.metadataKey = "UnifiedPropertyPanel.wide-flags";
    bool flags = false;
    registration.draw = [&](Properties::PropertyDrawerContext& context)
    {flags=context.property.kind==Inspector::InspectorPropertyKind::Flags;ImGui::TextUnformatted("Wide flags");context.interaction={true,false,true,true,false,1};context.changed=context.preview(type.GetEnum().FromBits(bits),"Wide flags");return true; };
    auto token = Properties::PropertyDrawerRegistry::Get().Register(registration);
    Inspector::ReflectedPropertyOptions options;
    options.metadata.emplace();
    options.metadata->flags = true;
    options.metadata->customDrawer = registration.metadataKey;
    auto value = Properties::Tests::WideFlags::Low;
    gui.Begin();
    const auto edit = panel.DrawValue("flags", "Flags", value, options);
    gui.End();
    EXPECT_TRUE(flags);
    EXPECT_TRUE(edit.changed);
    EXPECT_EQ(static_cast<uint64_t>(value), bits);
}

TEST(UnifiedPropertyPanel, NamedDrawerOverridesBroadMetadataHandlers)
{
    Inspector::InspectorRegistry storage;
    Properties::PropertyDrawerRegistry registry(storage);
    Properties::PropertyDrawerRegistration broad;
    broad.scope = Properties::DrawerScope::ExplicitMetadata;
    broad.identifier = "broad.flags";
    broad.metadataKey = "Flags";
    broad.draw = [](auto&)
    { return true; };
    auto a = registry.Register(broad);
    auto named = broad;
    named.identifier = "named.flags";
    named.metadataKey = "MyFlags";
    auto b = registry.Register(named);
    Inspector::PropertyNode node;
    node.metadata.flags = true;
    node.metadata.customDrawer = "MyFlags";
    ASSERT_TRUE(registry.Resolve(node));
    EXPECT_EQ(registry.Resolve(node)->identifier, "named.flags");
}
