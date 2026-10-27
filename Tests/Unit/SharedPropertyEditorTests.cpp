#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "Inspector/InspectorPropertyEditor.h"
#include "Inspector/InspectorRegistry.h"
#include "Inspector/InspectorProjectionBuilder.h"
#include "Inspector/InspectorView.h"
#include "Inspector/ReflectedPropertyEditor.h"
#include "Math/ExternalReflection.h"
#include "Materials/MaterialGraphView.h"
#include "VFX/Editor/VfxInspector.h"

namespace
{
using namespace NLS;
using namespace NLS::Editor;
using namespace NLS::Editor::Inspector;

struct Gui
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* current = ImGui::CreateContext();
    Gui()
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1000, 800};
        io.DeltaTime = 1.f / 60;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~Gui()
    {
        ImGui::DestroyContext(current);
        ImGui::SetCurrentContext(previous);
    }
    void Begin()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({900, 700});
        ImGui::Begin("Shared controls");
    }
    void End()
    {
        ImGui::End();
        ImGui::Render();
    }
};

Serialization::SerializedTargetSet Targets()
{
    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromAsset(InspectorTargetKind::AssetImporter, "shared", "0", "SharedControls"));
    return targets;
}

struct Request
{
    meta::Variant value;
    bool commit = true;
    size_t calls = 0;
    std::function<void(const InspectorPropertyDrawerContext&)> inspect;
};

std::shared_ptr<Request> Drawer(std::string key)
{
    auto request = std::make_shared<Request>();
    PropertyDrawerRegistration registration;
    registration.scope = InspectorDrawerScope::ExplicitMetadata;
    registration.metadataKey = key;
    registration.identifier = "SharedPropertyEditorTests." + key;
    registration.draw = [weak = std::weak_ptr(request)](InspectorPropertyDrawerContext& context)
    {
        const auto state = weak.lock();
        if (!state)
            return false;
        ++state->calls;
        if (state->inspect)
            state->inspect(context);
        ImGui::TextUnformatted("Shared registered drawer");
        context.changed = context.preview(state->value, "Edit shared value");
        if (state->commit)
            (void)context.commit();
        return true;
    };
    InspectorRegistry::Get().RegisterPropertyDrawer(std::move(registration));
    return request;
}
} // namespace

TEST(SharedPropertyEditor, InspectorAndEmbeddedValueUseTheSameRegisteredDrawer)
{
    Gui gui;
    const auto request = Drawer("shared-route");
    request->value = 17.5f;
    const auto lifetime = std::make_shared<int>();
    auto backend = std::make_shared<ReflectedValueBackend>(meta::Variant(1.f), lifetime);
    Serialization::SerializedObject object(Targets(), backend);
    auto properties = InspectorProjectionBuilder::BuildPropertyProjection(object, Targets(), {});
    ASSERT_EQ(properties.size(), 1u);
    properties.front().metadata.customDrawer = "shared-route";
    InspectorView inspector(false);
    gui.Begin();
    const auto first = inspector.DrawSerializedProperties(object, properties);
    gui.End();
    EXPECT_TRUE(first.applied);
    EXPECT_FLOAT_EQ(backend->Value().GetValue<float>(), 17.5f);
    ReflectedPropertyEditor embedded;
    embedded.SetLifetime(lifetime);
    ReflectedPropertyOptions options;
    options.layout = PropertyEditorLayout::ValueOnly;
    options.metadata.emplace();
    options.metadata->customDrawer = "shared-route";
    float value = 3.f;
    gui.Begin();
    const auto second = embedded.Draw("Scalar", "Scalar", value, options);
    gui.End();
    EXPECT_TRUE(second.changed);
    EXPECT_TRUE(second.completed);
    EXPECT_FLOAT_EQ(value, 17.5f);
    EXPECT_EQ(request->calls, 2u);
}

TEST(SharedPropertyEditor, ReadOnlyMetadataRejectsEvenARegisteredDrawerPreview)
{
    Gui gui;
    const auto request = Drawer("shared-readonly");
    request->value = 99.f;
    const auto lifetime = std::make_shared<int>();
    ReflectedPropertyEditor editor;
    editor.SetLifetime(lifetime);
    ReflectedPropertyOptions options;
    options.metadata.emplace();
    options.metadata->customDrawer = "shared-readonly";
    options.metadata->readOnly = true;
    float value = 7.f;
    gui.Begin();
    const auto edit = editor.Draw("Readonly", "Readonly", value, options);
    gui.End();
    EXPECT_FALSE(edit.changed);
    EXPECT_FLOAT_EQ(value, 7.f);
    EXPECT_EQ(request->calls, 1u);
}

TEST(SharedPropertyEditor, MetadataAndMixedTargetsShareOneStagedApply)
{
    Gui gui;
    const auto request = Drawer("shared-mixed");
    request->value = 2.f;
    bool mixed = false;
    request->inspect = [&](const InspectorPropertyDrawerContext& context)
    {
        ASSERT_NE(context.serializedProperty, nullptr);
        mixed = context.serializedProperty->HasMultipleDifferentValues();
        EXPECT_EQ(context.property.metadata.step, 0.25f);
    };
    const auto lifetime = std::make_shared<int>();
    ReflectedPropertyEditor editor;
    editor.SetLifetime(lifetime);
    ReflectedPropertyOptions options;
    options.metadata.emplace();
    options.metadata->customDrawer = "shared-mixed";
    options.metadata->step = 0.25f;
    options.metadata->rangeMin = 0.f;
    options.metadata->rangeMax = 1.f;
    std::vector<float> values{0.1f, 0.7f};
    gui.Begin();
    const auto edit = editor.DrawValues("Mixed", "Mixed", values, options);
    gui.End();
    ASSERT_TRUE(edit.changed);
    EXPECT_TRUE(mixed);
    EXPECT_EQ(values, (std::vector<float>{1.f, 1.f}));
}

TEST(SharedPropertyEditor, ReentrantDrawerRestoresTheOuterStreamAndApplyBoundary)
{
    Gui gui;
    auto outer = Drawer("shared-outer"), inner = Drawer("shared-inner");
    const auto lifetime = std::make_shared<int>();
    auto outerBackend = std::make_shared<ReflectedValueBackend>(meta::Variant(1.f), lifetime);
    auto innerBackend = std::make_shared<ReflectedValueBackend>(meta::Variant(2.f), lifetime);
    Serialization::SerializedObject outerObject(Targets(), outerBackend), innerObject(Targets(), innerBackend);
    auto outerRows = InspectorProjectionBuilder::BuildPropertyProjection(outerObject, Targets(), {});
    auto innerRows = InspectorProjectionBuilder::BuildPropertyProjection(innerObject, Targets(), {});
    ASSERT_EQ(outerRows.size(), 1u);
    ASSERT_EQ(innerRows.size(), 1u);
    outerRows[0].metadata.customDrawer = "shared-outer";
    innerRows[0].metadata.customDrawer = "shared-inner";
    InspectorPropertyEditor editor;
    PropertyEditorContext innerContext;
    innerContext.object = &innerObject;
    innerContext.apply = [&]
    { return innerObject.ApplyModifiedPropertiesWithoutUndo(); };
    inner->value = 22.f;
    outer->inspect = [&](const InspectorPropertyDrawerContext&)
    {
        ImGui::PushID("Nested");
        EXPECT_TRUE(editor.DrawProperties(innerRows, innerContext).applied);
        ImGui::PopID();
    };
    outer->value = 11.f;
    PropertyEditorContext outerContext;
    outerContext.object = &outerObject;
    outerContext.apply = [&]
    { return outerObject.ApplyModifiedPropertiesWithoutUndo(); };
    gui.Begin();
    const auto edit = editor.DrawProperties(outerRows, outerContext);
    gui.End();
    EXPECT_TRUE(edit.applied);
    EXPECT_FLOAT_EQ(outerBackend->Value().GetValue<float>(), 11.f);
    EXPECT_FLOAT_EQ(innerBackend->Value().GetValue<float>(), 22.f);
}

TEST(SharedPropertyEditor, VfxVectorsAndColorsUseTheSharedNativeTypesAndMetadata)
{
    Gui gui;
    VFXTools::EnsureVfxInspectorReflection();
    auto request = Drawer("shared-vfx");
    const auto lifetime = std::make_shared<int>();
    ReflectedPropertyEditor editor;
    editor.SetLifetime(lifetime);
    ReflectedPropertyOptions options;
    options.layout = PropertyEditorLayout::ValueOnly;
    options.metadata.emplace();
    options.metadata->customDrawer = "shared-vfx";
    VFX::Value value{};
    request->value = Maths::Vector3(1, 2, 3);
    request->inspect = [](const InspectorPropertyDrawerContext& context)
    { EXPECT_EQ(context.property.kind, InspectorPropertyKind::Vector3); };
    gui.Begin();
    const auto vectorEdit = VFXTools::DrawVfxValue(editor, "Vector", VFX::ValueType::Float3, value, options);
    gui.End();
    EXPECT_TRUE(vectorEdit.changed);
    EXPECT_EQ(value, (VFX::Value{1, 2, 3, 0}));
    options.metadata->colorUsage = true;
    request->value = Maths::Color(0.1f, 0.2f, 0.3f, 0.4f);
    request->inspect = [](const InspectorPropertyDrawerContext& context)
    { EXPECT_EQ(context.property.kind, InspectorPropertyKind::Color); };
    gui.Begin();
    const auto colorEdit = VFXTools::DrawVfxValue(editor, "Color", VFX::ValueType::Float4, value, options);
    gui.End();
    EXPECT_TRUE(colorEdit.changed);
    EXPECT_EQ(value, (VFX::Value{0.1f, 0.2f, 0.3f, 0.4f}));
}

TEST(SharedPropertyEditor, EscapeDiscardsTheStagedValueWithoutApplyingTheHost)
{
    Gui gui;
    auto request = Drawer("shared-cancel");
    request->value = 9.f;
    request->commit = false;
    const auto lifetime = std::make_shared<int>();
    auto backend = std::make_shared<ReflectedValueBackend>(meta::Variant(1.f), lifetime);
    Serialization::SerializedObject object(Targets(), backend);
    auto rows = InspectorProjectionBuilder::BuildPropertyProjection(object, Targets(), {});
    ASSERT_EQ(rows.size(), 1u);
    rows[0].metadata.customDrawer = "shared-cancel";
    InspectorPropertyEditor editor;
    PropertyEditorContext context;
    context.object = &object;
    size_t applies = 0;
    context.apply = [&]
    {++applies;return object.ApplyModifiedPropertiesWithoutUndo(); };
    gui.Begin();
    const auto first = editor.DrawProperties(rows, context);
    gui.End();
    EXPECT_FALSE(first.changedPaths.empty());
    EXPECT_TRUE(object.HasModifiedProperties());
    EXPECT_FLOAT_EQ(backend->Value().GetValue<float>(), 1.f);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    gui.Begin();
    const auto canceled = editor.FinishItem(rows[0], context);
    gui.End();
    EXPECT_TRUE(canceled.canceled);
    EXPECT_FALSE(object.HasModifiedProperties());
    EXPECT_EQ(applies, 0u);
    EXPECT_FLOAT_EQ(object.FindProperty("Value").GetValue().GetValue<float>(), 1.f);
}

TEST(SharedPropertyEditor, ApplyingThePrimaryValueStillUpdatesOtherMixedTargets)
{
    Gui gui;
    auto request = Drawer("shared-primary");
    request->value = 1.f;
    const auto lifetime = std::make_shared<int>();
    ReflectedPropertyEditor editor;
    editor.SetLifetime(lifetime);
    ReflectedPropertyOptions options;
    options.metadata.emplace();
    options.metadata->customDrawer = "shared-primary";
    std::vector<float> values{1.f, 2.f};
    gui.Begin();
    const auto edit = editor.DrawValues("Primary", "Primary", values, options);
    gui.End();
    EXPECT_TRUE(edit.changed);
    EXPECT_TRUE(edit.completed);
    EXPECT_EQ(values, (std::vector<float>{1.f, 1.f}));
}

TEST(SharedPropertyEditor, ShaderGraphDetailsUseTheRegisteredDrawerAndOneUndoGesture)
{
    Gui gui;
    auto request = std::make_shared<Request>();
    PropertyDrawerRegistration registration;
    registration.scope = InspectorDrawerScope::ExactType;
    registration.valueType = NLS_TYPEOF(float);
    registration.identifier = "SharedPropertyEditorTests.GraphDisplacement";
    registration.draw = [weak = std::weak_ptr(request)](InspectorPropertyDrawerContext& context)
    {
        const auto state = weak.lock();
        if (!state || context.property.displayName != "Maximum world displacement")
            return false;
        ++state->calls;
        ImGui::TextUnformatted("Shared graph displacement");
        const auto gestureId=ImGui::GetID("SharedGraphDrag");
        if(!state->commit)
        {
            ImGui::SetActiveID(gestureId,ImGui::GetCurrentWindow());
            ImGui::KeepAliveID(gestureId);
        }
        else ImGui::ClearActiveID();
        context.changed = context.preview(state->value, "Set graph displacement");
        if (state->commit)
            (void)context.commit();
        return true;
    };
    InspectorRegistry::Get().RegisterPropertyDrawer(std::move(registration));
    Undo::EditorUndoService undo;
    Materials::SourceTools::MaterialSourceDocument document(&undo);
    auto graph = NLS::MaterialCompiler::CreateDefaultMaterialGraph();
    document.New(NLS::MaterialCompiler::SerializeMaterialGraph(graph));
    const auto before = document.Asset();
    Materials::MaterialGraphView view;
    for (size_t index = 0; index < 3; ++index)
    {
        request->value = static_cast<float>(index + 1);
        request->commit = index == 2;
        gui.Begin();
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        view.Draw(document);
        gui.End();
    }
    EXPECT_EQ(request->calls, 3u);
    std::string error;
    ASSERT_TRUE(NLS::MaterialCompiler::DeserializeMaterialGraph(document.Asset(), graph, error)) << error;
    EXPECT_FLOAT_EQ(graph.maximumWorldDisplacement, 3.f);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_EQ(document.Asset(), before);
    EXPECT_FALSE(undo.CanUndo());
}

TEST(SharedPropertyEditor, NarrowColorPickerStaysInsideThePropertyValueColumn)
{
    Gui gui;const auto lifetime=std::make_shared<int>();
    auto backend=std::make_shared<ReflectedValueBackend>(meta::Variant(Maths::Color(1,0.5f,0.1f,1)),lifetime);
    Serialization::SerializedObject object(Targets(),backend);
    auto rows=InspectorProjectionBuilder::BuildPropertyProjection(object,Targets(),{});
    ASSERT_EQ(rows.size(),1u);
    InspectorPropertyEditor editor;PropertyEditorContext context;context.object=&object;
    gui.Begin();ImGui::BeginChild("NarrowValue",{180,300});
    const auto left=ImGui::GetCursorScreenPos().x;
    const auto width=ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(width);
    (void)editor.DrawProperty(rows[0],context,PropertyEditorLayout::ValueOnly);
    EXPECT_LE(ImGui::GetItemRectMax().x,left+width+1.f);
    EXPECT_GT(ImGui::GetItemRectMin().x,left+width*0.5f);
    ImGui::EndChild();gui.End();
}

TEST(SharedPropertyEditor, ReflectedDetailsCannotReintroduceTheirOwnTypedControls)
{
    const auto read = [](const char* path)
    {std::ifstream file(std::filesystem::path(NLS_ROOT_DIR)/path);return std::string(std::istreambuf_iterator<char>(file),{}); };
    const auto view = read("Project/Editor/Inspector/InspectorView.cpp");
    EXPECT_EQ(view.find("switch (property.kind)"), std::string::npos);
    EXPECT_EQ(view.find("DrawLabeledFloatComponents"), std::string::npos);
    const auto embedded = read("Project/Editor/Inspector/ReflectedPropertyEditor.cpp");
    EXPECT_EQ(embedded.find("InspectorView view"), std::string::npos);
    EXPECT_NE(embedded.find("InspectorPropertyEditor properties"), std::string::npos);
    const auto vfx = read("Project/Editor/Panels/VfxEditor.cpp");
    EXPECT_EQ(vfx.find("VFXTools::DrawValue("), std::string::npos);
    EXPECT_EQ(vfx.find("DragScalarN("), std::string::npos);
    const auto material = read("Project/Editor/Panels/MaterialEditor.cpp");
    const auto parameters = material.substr(material.find("void MaterialEditor::DrawMaterial("));
    for (const auto* control : {"ImGui::Drag", "ImGui::Input", "ImGui::Slider", "ImGui::ColorEdit", "ImGui::Checkbox"})
        EXPECT_EQ(parameters.find(control), std::string::npos) << control;
    const auto designer = read("Project/Editor/UI/UIDocumentInspectorView.cpp");
    EXPECT_EQ(designer.find("ImGui::DragFloat"), std::string::npos);
    const auto graph = read("Project/Editor/Materials/MaterialGraphView.cpp");
    for (const auto* control : {"ImGui::DragFloat", "ImGui::ColorEdit", "ImGui::Combo", "ImGui::Checkbox", "ImGui::InputInt"})
        EXPECT_EQ(graph.find(control), std::string::npos) << control;
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/VFX/Editor/VfxValueWidgets.h"));
}
