#include <gtest/gtest.h>
#include <climits>
#include <Guid.h>
#include <VFXCompiler/VfxStackCompiler.h>
#include "VFX/Editor/VfxInspector.h"
#include "Inspector/InspectorProjectionBuilder.h"
#include "Inspector/InspectorView.h"
#include "Inspector/InspectorRegistry.h"
#include "Inspector/EditorSelectionService.h"
#include <imgui.h>

namespace
{
using namespace NLS;
using namespace NLS::Editor;
using namespace NLS::Editor::VFXTools;
namespace author = NLS::VFX::Authoring;
Serialization::SerializedTargetSet Targets()
{
    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(Inspector::InspectorTargetHandle::FromAsset(
        Inspector::InspectorTargetKind::AssetImporter,"Particle Properties",{},"VfxInspectorTest"));
    return targets;
}
}

TEST(VfxInspector, ReflectedFieldsEnumsAndMetadataGenerateTheSharedProjection)
{
    EnsureVfxInspectorReflection(); auto lifetime = std::make_shared<int>();
    VfxEmitterDetails emitter; emitter.name = "Reflected"; emitter.target = VFX::SimulationTarget::GPU; emitter.capacity = 32768;
    auto backend = std::make_shared<Inspector::ReflectedValueBackend>(meta::Variant(emitter),lifetime);
    Serialization::SerializedObject object(Targets(),backend); ASSERT_TRUE(object.IsValid()) << object.GetLastError();
    ASSERT_EQ(NLS_TYPEOF(VfxEmitterDetails).GetFields().size(),6u);
    const auto target = object.FindProperty("Value.target"); ASSERT_TRUE(target);
    EXPECT_EQ(target.GetPropertyType(),Serialization::SerializedPropertyType::Enum);
    ASSERT_EQ(target.GetEnumDisplayNames().size(),2u);
    auto rows = Inspector::InspectorProjectionBuilder::BuildPropertyProjection(object,Targets(),{});
    ASSERT_EQ(rows.size(),1u); ASSERT_EQ(rows[0].children.size(),6u);
    EXPECT_EQ(rows[0].children[2].kind,Inspector::InspectorPropertyKind::Enum);
    EXPECT_EQ(rows[0].children[2].displayName,"Sim Target"); EXPECT_EQ(rows[0].children[3].displayName,"Max Particles");
    ASSERT_TRUE(object.FindProperty("Value.capacity").SetUnsignedLongValue(65536));
    ASSERT_TRUE(object.ApplyModifiedPropertiesWithoutUndo()) << object.GetLastError();
    EXPECT_EQ(backend->Value().GetValue<VfxEmitterDetails>().capacity,65536u);
    EXPECT_EQ(backend->Value().GetValue<VfxEmitterDetails>().target,VFX::SimulationTarget::GPU);
}

TEST(VfxInspector, ConditionalVisibilityAndReadOnlyFlagsUseTheCanonicalTypeTree)
{
    EnsureVfxInspectorReflection(); auto lifetime = std::make_shared<int>();
    Inspector::ReflectedPropertyOptions options; options.disabled = {"fixedTickDelta"}; options.hidden = {"warmupTickCount","warmupTickDelta"};
    auto backend = std::make_shared<Inspector::ReflectedValueBackend>(meta::Variant(VfxSystemDetails{}),lifetime,options);
    Serialization::SerializedObject object(Targets(),backend); ASSERT_TRUE(object.IsValid()) << object.GetLastError();
    const auto delta = object.FindProperty("Value.fixedTickDelta"); ASSERT_TRUE(delta); EXPECT_TRUE(delta.IsReadOnly());
    EXPECT_FALSE(delta.SetDoubleValue(0.125));
    const auto rows = Inspector::InspectorProjectionBuilder::BuildPropertyProjection(object,Targets(),{});
    ASSERT_EQ(rows.size(),1u); EXPECT_EQ(rows[0].children.size(),3u);
    ASSERT_TRUE(object.FindProperty("Value.fixedTick").SetBoolValue(true));
    ASSERT_TRUE(object.ApplyModifiedPropertiesWithoutUndo()) << object.GetLastError();
    const auto value = backend->Value().GetValue<VfxSystemDetails>(); EXPECT_TRUE(value.fixedTick);
    EXPECT_EQ(value.warmupTickCount,0u); EXPECT_DOUBLE_EQ(value.warmupTickDelta,1.0/15);
    auto invalid = value; invalid.fixedTickDelta = 0.125;
    ASSERT_TRUE(object.FindProperty("Value").SetValue(meta::Variant(invalid)));
    EXPECT_FALSE(object.ApplyModifiedPropertiesWithoutUndo());
    EXPECT_DOUBLE_EQ(backend->Value().GetValue<VfxSystemDetails>().fixedTickDelta,value.fixedTickDelta);
}

TEST(VfxInspector, NativeScalarStreamsPreserveInt32AndBoolAndRejectClosedTargets)
{
    EnsureVfxInspectorReflection(); auto lifetime = std::make_shared<int>();
    for (const auto initial : {INT32_MIN,INT32_MAX,16777217})
    {
        auto backend = std::make_shared<Inspector::ReflectedValueBackend>(meta::Variant(int32_t(initial)),lifetime);
        Serialization::SerializedObject object(Targets(),backend); ASSERT_TRUE(object.IsValid()) << object.GetLastError();
        auto property = object.FindProperty("Value"); ASSERT_TRUE(property); EXPECT_EQ(property.GetLongValue(),initial);
        ASSERT_TRUE(property.SetLongValue(initial == INT32_MIN ? INT32_MAX : INT32_MIN));
        ASSERT_TRUE(object.ApplyModifiedPropertiesWithoutUndo()) << object.GetLastError();
        EXPECT_EQ(backend->Value().GetValue<int32_t>(),initial == INT32_MIN ? INT32_MAX : INT32_MIN);
    }
    auto backend = std::make_shared<Inspector::ReflectedValueBackend>(meta::Variant(true),lifetime);
    Serialization::SerializedObject object(Targets(),backend); ASSERT_TRUE(object.FindProperty("Value").SetBoolValue(false));
    ASSERT_TRUE(object.ApplyModifiedPropertiesWithoutUndo()); EXPECT_FALSE(backend->Value().GetValue<bool>());
    ASSERT_TRUE(object.FindProperty("Value").SetBoolValue(true)); lifetime.reset();
    EXPECT_FALSE(object.ApplyModifiedPropertiesWithoutUndo()); EXPECT_FALSE(backend->Value().GetValue<bool>());
}

TEST(VfxInspector, ContinuousChangesCancelAndUndoRemainDocumentScopedAndSaveReopens)
{
    EnsureVfxInspectorReflection(); Undo::EditorUndoService undo; VfxDocument document(&undo),other;
    const auto before = author::Serialize(document.Asset());
    Inspector::ReflectedPropertyEdit preview; preview.changed = true; preview.paths = {"Value.capacity"};
    for (const auto capacity : {20000u,30000u,40000u})
    {
        auto candidate = document.Asset(); candidate.emitters[0].settings.capacity = capacity;
        ASSERT_TRUE(CommitInspectorEdit(document,candidate,preview,"Set reflected capacity"));
    }
    Inspector::ReflectedPropertyEdit complete; complete.completed = true;
    ASSERT_TRUE(CommitInspectorEdit(document,document.Asset(),complete,"Finish capacity"));
    EXPECT_EQ(document.Asset().emitters[0].settings.capacity,40000u); EXPECT_NE(other.Asset().emitters[0].settings.capacity,40000u);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_EQ(author::Serialize(document.Asset()),before); EXPECT_FALSE(undo.CanUndo());
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); EXPECT_EQ(document.Asset().emitters[0].settings.capacity,40000u);
    auto candidate = document.Asset(); candidate.emitters[0].settings.capacity = 50000u;
    ASSERT_TRUE(CommitInspectorEdit(document,candidate,preview,"Cancel reflected capacity"));
    Inspector::ReflectedPropertyEdit cancel; cancel.canceled = true;
    ASSERT_TRUE(CommitInspectorEdit(document,document.Asset(),cancel,"Cancel")); EXPECT_EQ(document.Asset().emitters[0].settings.capacity,40000u);
    const auto path = std::filesystem::temp_directory_path()/("NullusReflectedVfx_"+Guid::New().ToString()+".vfx");
    struct Cleanup {std::filesystem::path path; ~Cleanup(){std::error_code error; std::filesystem::remove(path,error);}} cleanup{path};
    ASSERT_TRUE(document.Save(path)); VfxDocument loaded; ASSERT_TRUE(loaded.Load(path));
    EXPECT_EQ(loaded.Asset().emitters[0].settings.capacity,40000u);
    auto staleBackend = std::make_shared<Inspector::ReflectedValueBackend>(meta::Variant(VfxEmitterDetails{}),document.Lifetime());
    document.New(); EXPECT_FALSE(staleBackend->Alive());
    const auto source = author::Serialize(candidate); author::System reopened; std::string error;
    ASSERT_TRUE(author::Deserialize(source,reopened,error)) << error; EXPECT_EQ(reopened.emitters[0].settings.capacity,50000u);
}

TEST(VfxInspector, EmbeddedControlsUseRegisteredInspectorDrawersAndCommitOneDocumentUndo)
{
    EnsureVfxInspectorReflection();
    struct DrawerState { uint32_t requested = 20000; bool commit = false; size_t calls = 0; };
    auto drawer = std::make_shared<DrawerState>();
    Inspector::PropertyDrawerRegistration registration;
    registration.scope = Inspector::InspectorDrawerScope::Field;
    registration.identifier = "VfxInspectorTest.SharedCapacity";
    registration.fieldPath = Inspector::PropertyPath("Value.capacity");
    registration.draw = [weak = std::weak_ptr<DrawerState>(drawer)](Inspector::InspectorPropertyDrawerContext& context)
    {
        auto state = weak.lock(); if (!state) return false;
        ++state->calls; ImGui::TextUnformatted("Capacity");
        uint32_t value = state->requested; context.changed = context.preview(meta::Variant(value),"Set capacity");
        if (state->commit) EXPECT_TRUE(context.commit());
        return true;
    };
    Inspector::InspectorRegistry::Get().RegisterPropertyDrawer(std::move(registration));
    auto* previous = ImGui::GetCurrentContext(); auto* context = ImGui::CreateContext();
    struct Restore {ImGuiContext *context,*previous; ~Restore(){ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous);}} restore{context,previous};
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {1000,800}; io.DeltaTime = 1.0f/60;
    unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    Undo::EditorUndoService undo; VfxDocument document(&undo); Inspector::ReflectedPropertyEditor editor;
    editor.SetLifetime(document.Lifetime()); const auto originalSelection = Inspector::EditorSelectionService::Get().GetSelection();
    const auto initial = document.Asset().emitters[0].settings.capacity;
    for (uint32_t i = 0; i < 3; ++i)
    {
        drawer->requested = 20000+i*10000; drawer->commit = i == 2;
        auto source = document.Asset(); const auto& emitter = source.emitters[0];
        VfxEmitterDetails fields{emitter.settings.name,emitter.enabled,emitter.settings.target,emitter.settings.capacity,emitter.settings.sortByViewDepth,emitter.settings.frustumCulling};
        ImGui::NewFrame(); ImGui::SetNextWindowSize({800,600}); ImGui::Begin("Shared Vfx controls");
        const auto edit = editor.Draw("Emitter","Emitter",fields);
        source.emitters[0].settings.capacity = fields.capacity;
        EXPECT_TRUE(edit.changed); EXPECT_EQ(edit.completed,i == 2);
        EXPECT_TRUE(CommitInspectorEdit(document,source,edit,"Edit reflected capacity"));
        ImGui::End(); ImGui::Render();
        EXPECT_EQ(document.Asset().emitters[0].settings.capacity,drawer->requested);
    }
    EXPECT_EQ(drawer->calls,3u); EXPECT_EQ(Inspector::EditorSelectionService::Get().GetSelection(),originalSelection);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_EQ(document.Asset().emitters[0].settings.capacity,initial); EXPECT_FALSE(undo.CanUndo());
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); EXPECT_EQ(document.Asset().emitters[0].settings.capacity,40000u);
}
