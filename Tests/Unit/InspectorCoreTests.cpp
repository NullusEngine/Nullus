#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include <Components/CameraComponent.h>
#include <Components/LightComponent.h>
#include <Components/UICanvasComponent.h>
#include <Engine/UI/UIDocumentSourceResolver.h>
#include <GameObject.h>
#include <Math/Vector3.h>
#include <Reflection/ExternalReflectionRegistration.h>
#include <Rendering/Resources/Material.h>
#include <Guid.h>
#include <Reflection/Macros.h>
#include <Reflection/ReflectionDatabase.h>
#include <SceneSystem/Scene.h>
#include <Serialize/PPtr.h>
#include <ServiceLocator.h>
#include <Scripting/ScriptComponent.h>
#include <Scripting/ScriptTypes.h>

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/PrefabEditorWorkflow.h"
#include "Inspector/AssetTargetAdapter.h"
#include "Inspector/ComponentEditor.h"
#include "Inspector/EditorExtensionRegistry.h"
#include "Inspector/UICanvasComponentEditor.h"
#include "Inspector/ComponentStructureUndo.h"
#include "Inspector/InspectorUndo.h"
#include "Inspector/InspectorRegistry.h"
#include "Inspector/InspectorLayout.h"
#include "Inspector/InspectorProjectionBuilder.h"
#include "Inspector/InspectorView.h"
#include "Inspector/NativeReflectionAdapter.h"
#include "Inspector/ObjectReferenceSupport.h"
#include "Inspector/PrefabInspectorSupport.h"
#include "Inspector/PropertyTypeTraits.h"
#include "Inspector/ProjectSettingsTargetAdapter.h"
#include "Inspector/ScriptSerializedAdapter.h"
#include "Serialization/AccessorSerializedBackend.h"
#include "Serialization/ImporterDraftSerializedBackend.h"
#include "Serialization/MaterialSerializedBackend.h"
#include "Serialization/ProjectSettingsSerializedBackend.h"
#include "Serialization/SerializedObject.h"
#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsRegistry.h"
#include "Undo/EditorUndoMutationContext.h"
#include "Panels/AssetBrowser.h"
#include "EditorProject_MetaGenerated.h"

namespace NLS::Editor::Inspector::Tests
{
struct StableReferenceFixture
{
    static constexpr const char* TypeName()
    {
        return "NLS::Editor::Inspector::Tests::StableReferenceFixture";
    }

    NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject> primary;
    NLS::Engine::GameObject* secondary = nullptr;
    NLS::Engine::Components::LightComponent* component = nullptr;
    NLS::Array<NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>> references;
};

NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Inspector::Tests::StableReferenceFixture)

inline void RegisterStableReferenceFixtureReflection(
    NLS::meta::ReflectionDatabase& db,
    NLS::meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Inspector::Tests::StableReferenceFixture)
        NLS_META_EXTERNAL_FIELD(
            NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>,
            primary);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::GameObject*, secondary);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Components::LightComponent*, component);
        NLS_META_EXTERNAL_FIELD(
            NLS::Array<NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>>,
            references);
    NLS_META_EXTERNAL_END();
}
} // namespace NLS::Editor::Inspector::Tests

NLS_META_EXTERNAL_MODULE(NLS::Editor::Inspector::Tests::RegisterStableReferenceFixtureReflection)

namespace NLS::Editor::Inspector::Tests
{
namespace
{
class ScopedInspectorUndoService final
{
public:
    ScopedInspectorUndoService()
    {
        NLS::Core::ServiceLocator::Provide(m_service);
    }

    ~ScopedInspectorUndoService()
    {
        NLS::Core::ServiceLocator::Remove<Undo::EditorUndoService>();
    }

    [[nodiscard]] Undo::EditorUndoService& Get() { return m_service; }

private:
    Undo::EditorUndoService m_service;
};

struct StaleCommandProbe
{
    bool alive = true;
};

// Commands used by the production Inspector retain structured target data.
// Keep this test probe explicit too, rather than restoring the removed
// callback-command escape hatch just to exercise stale-history handling.
class StaleInspectorProbeCommand final : public Undo::IEditorUndoCommand
{
public:
    StaleInspectorProbeCommand(Undo::UndoCommandInfo info, std::shared_ptr<StaleCommandProbe> probe)
        : m_info(std::move(info))
        , m_probe(std::move(probe))
    {
    }

    [[nodiscard]] Undo::UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] Undo::UndoValidationResult Validate(
        Undo::UndoDirection,
        const Undo::UndoExecutionContext&) const override
    {
        return m_probe != nullptr && m_probe->alive
            ? Undo::UndoValidationResult::Ready()
            : Undo::UndoValidationResult::Stale("Destroyed for test.");
    }

    Undo::UndoStartResult Apply(
        Undo::UndoDirection,
        const Undo::UndoExecutionContext&,
        Undo::UndoCompletion) override
    {
        return Undo::UndoStartResult::Completed();
    }

    bool TryMergeFrom(const Undo::IEditorUndoCommand&) override
    {
        return false;
    }

private:
    Undo::UndoCommandInfo m_info;
    std::shared_ptr<StaleCommandProbe> m_probe;
};

class TemporaryInspectorAssetProject final
{
public:
    TemporaryInspectorAssetProject()
        : root(std::filesystem::temp_directory_path() /
            ("NullusInspectorPrefabUndo_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(root / "Assets");
    }

    ~TemporaryInspectorAssetProject()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    std::filesystem::path root;
};

std::string ReadTextFileForTesting(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

TEST(InspectorCoreTests, PropertyPathBuildsStableNestedSegments)
{
    const PropertyPath path("GameObject");
    EXPECT_EQ(path.Field("components").Index(2u).Field("enabled").ToString(),
        "GameObject.components[2].enabled");
    EXPECT_TRUE(PropertyPath {}.Empty());
}

TEST(InspectorCoreTests, SerializedEditSessionStagesAndCommitsThroughByteStreamUndo)
{
    ScopedInspectorUndoService undoScope;
    Engine::GameObject object("Before");
    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(object, InspectorTargetKind::GameObject));
    targets.Normalize();

    Serialization::SerializedObject serialized(
        targets,
        Serialization::AccessorSerializedBackend::Create());
    ASSERT_TRUE(serialized.IsValid()) << serialized.GetLastError();

    auto cancelled = serialized.BeginEditSession("GameObject.name");
    ASSERT_TRUE(cancelled.Preview(meta::Variant(std::string {"Discarded"})));
    EXPECT_EQ(object.GetName(), "Before");
    EXPECT_TRUE(cancelled.Cancel());
    EXPECT_EQ(object.GetName(), "Before");
    EXPECT_FALSE(serialized.HasModifiedProperties());

    auto committed = serialized.BeginEditSession("GameObject.name");
    ASSERT_TRUE(committed.Preview(meta::Variant(std::string {"After"})));
    EXPECT_EQ(object.GetName(), "Before");
    ASSERT_TRUE(committed.Commit()) << serialized.GetLastError();
    EXPECT_EQ(object.GetName(), "After");
    ASSERT_TRUE(undoScope.Get().RequestUndo().Succeeded());
    EXPECT_EQ(object.GetName(), "Before");
    ASSERT_TRUE(undoScope.Get().RequestRedo().Succeeded());
    EXPECT_EQ(object.GetName(), "After");
}

TEST(InspectorCoreTests, RegistryUsesDocumentedDrawerPrecedenceAndRejectsDuplicates)
{
    auto& registry = InspectorRegistry::Get();
    const PropertyPath fieldPath("InspectorCoreTests.Precedence.value");
    registry.RegisterPropertyDrawer({InspectorDrawerScope::ReadOnlyFallback, "InspectorCoreFallback", {}, {}, {}});
    registry.RegisterPropertyDrawer({InspectorDrawerScope::ExactType, "InspectorCoreExact", {}, {}, NLS_TYPEOF(int32_t)});
    registry.RegisterPropertyDrawer({InspectorDrawerScope::ExplicitMetadata, "InspectorCoreFlags", {}, "Flags", {}});
    registry.RegisterPropertyDrawer({InspectorDrawerScope::Field, "InspectorCoreField", fieldPath, {}, {}});

    PropertyNode node;
    node.path = fieldPath;
    node.valueType = NLS_TYPEOF(int32_t);
    node.metadata.flags = true;
    ASSERT_TRUE(registry.ResolvePropertyDrawer(node).has_value());
    EXPECT_EQ(registry.ResolvePropertyDrawer(node)->identifier, "InspectorCoreField");

    node.path = PropertyPath("InspectorCoreTests.Precedence.other");
    ASSERT_TRUE(registry.ResolvePropertyDrawer(node).has_value());
    EXPECT_EQ(registry.ResolvePropertyDrawer(node)->identifier, "InspectorCoreFlags");

    node.metadata.flags = false;
    ASSERT_TRUE(registry.ResolvePropertyDrawer(node).has_value());
    EXPECT_EQ(registry.ResolvePropertyDrawer(node)->identifier, "InspectorCoreExact");

    node.valueType = NLS_TYPEOF(std::string);
    ASSERT_TRUE(registry.ResolvePropertyDrawer(node).has_value());
    EXPECT_EQ(registry.ResolvePropertyDrawer(node)->identifier, "InspectorCoreFallback");

    EXPECT_THROW(
        registry.RegisterPropertyDrawer({InspectorDrawerScope::Field, "InspectorCoreDuplicate", fieldPath, {}, {}}),
        std::logic_error);
}

TEST(InspectorCoreTests, MetadataAndUnsignedTypesUseInspectorContracts)
{
    EditorPropertyMetadata metadata;
    metadata.displayName = "Initial Value";
    metadata.tooltip = "A test-only tooltip";
    InspectorMetadataRegistry::Get().Register("InspectorCoreMetadataType", "value", metadata);

    const auto resolved = BuildEditorPropertyMetadata(
        "InspectorCoreMetadataType",
        "value",
        {});
    EXPECT_EQ(resolved.displayName, "Initial Value");
    EXPECT_EQ(resolved.tooltip, "A test-only tooltip");
    EXPECT_EQ(GetInspectorPropertyKind(NLS_TYPEOF(uint64_t)), InspectorPropertyKind::UnsignedInteger64);
    EXPECT_EQ(GetInspectorPropertyKind(NLS_TYPEOF(unsigned int)), InspectorPropertyKind::UnsignedInteger32);
}

TEST(InspectorCoreTests, ScriptFieldDescriptorsUseTheSameDeclarativeMetadataContract)
{
    Scripting::ScriptFieldDescriptor descriptor;
    descriptor.name = "movementSpeed";
    descriptor.metadata.displayName = "Movement Speed";
    descriptor.metadata.tooltip = "World units per second.";
    descriptor.metadata.header = "Motion";
    descriptor.metadata.spaceBefore = 4.0f;
    descriptor.metadata.rangeMin = 0.0f;
    descriptor.metadata.rangeMax = 20.0f;
    descriptor.metadata.min = 0.0f;
    descriptor.metadata.max = 100.0f;
    descriptor.metadata.multiline = true;
    descriptor.metadata.readOnly = true;
    descriptor.metadata.hideInInspector = true;
    descriptor.metadata.flags = true;
    descriptor.metadata.colorUsage = true;
    descriptor.metadata.referenceTypeConstraint = "NLS::Engine::GameObject";

    const auto metadata = BuildEditorPropertyMetadata(descriptor, "Tests.ScriptMetadata");
    EXPECT_EQ(metadata.displayName, descriptor.metadata.displayName);
    EXPECT_EQ(metadata.tooltip, descriptor.metadata.tooltip);
    EXPECT_EQ(metadata.header, descriptor.metadata.header);
    EXPECT_FLOAT_EQ(metadata.spaceBefore, descriptor.metadata.spaceBefore);
    EXPECT_EQ(metadata.rangeMin, descriptor.metadata.rangeMin);
    EXPECT_EQ(metadata.rangeMax, descriptor.metadata.rangeMax);
    EXPECT_EQ(metadata.min, descriptor.metadata.min);
    EXPECT_EQ(metadata.max, descriptor.metadata.max);
    EXPECT_TRUE(metadata.multiline);
    EXPECT_TRUE(metadata.readOnly);
    EXPECT_TRUE(metadata.hideInInspector);
    EXPECT_TRUE(metadata.flags);
    EXPECT_TRUE(metadata.colorUsage);
    EXPECT_EQ(metadata.referenceTypeConstraint, descriptor.metadata.referenceTypeConstraint);
}

TEST(InspectorCoreTests, ScriptComponentsUseConcreteAssetTitlesAndRefreshTheirSchema)
{
    Scripting::ScriptApiDatabase api;
    Scripting::ScriptTypeDescriptor rotatingCube;
    rotatingCube.id = 101u;
    rotatingCube.name = "Gameplay.RotatingCube";
    Scripting::ScriptFieldDescriptor angle;
    angle.id = 1001u;
    angle.name = "angle";
    angle.type = {1002u, Scripting::ScriptValueKind::Float, "float", sizeof(float), alignof(float), true};
    rotatingCube.fields.push_back(angle);
    ASSERT_TRUE(api.RegisterType(rotatingCube));

    Scripting::ScriptTypeDescriptor flyController;
    flyController.id = 202u;
    flyController.name = "Gameplay.FlyController";
    Scripting::ScriptFieldDescriptor speed;
    speed.id = 2001u;
    speed.name = "speed";
    speed.type = {2002u, Scripting::ScriptValueKind::Int32, "int", sizeof(int32_t), alignof(int32_t), true};
    flyController.fields.push_back(speed);
    ASSERT_TRUE(api.RegisterType(flyController));

    Scripting::ScriptRuntime runtime;
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());
    Engine::GameObject object("Script Inspector Target");
    auto* script = object.AddComponent<Scripting::ScriptComponent>();
    ASSERT_NE(script, nullptr);
    script->SetRuntime(&runtime);

    Scripting::ScriptAsset csharpAsset;
    csharpAsset.language = Scripting::ScriptLanguage::CSharp;
    csharpAsset.sourcePath = "Assets/Scripts/RotatingCube.cs";
    csharpAsset.scriptType = rotatingCube.id;
    csharpAsset.isComponent = true;
    script->SetScriptAsset(csharpAsset);
    ASSERT_TRUE(script->SetSerializedField(angle.id, 45.0f));

    // Primitive script fields now use the canonical byte stream directly. The
    // ScriptComponent is resolved only through its durable handle; no
    // Inspector chrome is metadata-only; no binding is involved in either
    // readback or writeback.
    Serialization::SerializedTargetSet scriptTargets;
    scriptTargets.targets = {InspectorTargetHandle::FromObject(*script, InspectorTargetKind::NativeObject)};
    scriptTargets.Normalize();
    ASSERT_TRUE(Serialization::AccessorSerializedBackend::Supports(scriptTargets));
    Serialization::SerializedObject scriptSerialized(
        scriptTargets,
        Serialization::AccessorSerializedBackend::Create());
    const auto serializedAngle = scriptSerialized.FindProperty("angle");
    ASSERT_TRUE(serializedAngle);
    EXPECT_FLOAT_EQ(serializedAngle.GetFloatValue(), 45.0f);
    ASSERT_TRUE(serializedAngle.SetFloatValue(90.0f));
    ASSERT_TRUE(scriptSerialized.ApplyModifiedPropertiesWithoutUndo());
    const auto updatedAngle = script->FindSerializedField(angle.id);
    ASSERT_NE(updatedAngle, nullptr);
    EXPECT_FLOAT_EQ(std::get<float>(*updatedAngle), 90.0f);

    InspectorTargetSet targets;
    targets.targets = {InspectorTargetHandle::FromObject(object, InspectorTargetKind::GameObject)};
    NativeReflectionAdapter adapter;
    const auto csharpVersion = adapter.GetStructureVersion(targets);
    const auto csharpPresentation = adapter.BuildPresentation(targets, {});
    // Projection metadata is deliberately read-only. All serialized reads and
    // writes below use the canonical byte stream, never the UI projection.
    ASSERT_EQ(csharpPresentation.roots.size(), 1u);
    const auto findScriptNode = [](const InspectorPresentation& presentation) -> const PropertyNode*
    {
        if (presentation.roots.empty())
            return nullptr;
        const auto found = std::find_if(presentation.roots.front().children.begin(), presentation.roots.front().children.end(), [](const PropertyNode& node)
        {
            return dynamic_cast<const Scripting::ScriptComponent*>(NativeReflectionAdapter::ResolveObject(node.target)) != nullptr;
        });
        return found == presentation.roots.front().children.end() ? nullptr : &(*found);
    };
    const auto* csharpNode = findScriptNode(csharpPresentation);
    ASSERT_NE(csharpNode, nullptr);
    EXPECT_EQ(csharpNode->displayName, "Rotating Cube (C#)");
    // NativeReflectionAdapter owns only the component-card projection. Script
    // fields must come from the real SerializedProperty cursor sequence, not
    // from a script-specific presentation builder.
    EXPECT_EQ(std::find_if(csharpNode->children.begin(), csharpNode->children.end(), [](const PropertyNode& node)
    {
        return node.fieldName == "scriptAsset" || node.fieldName == "angle";
    }), csharpNode->children.end());

    const auto csharpProperties = InspectorProjectionBuilder::BuildPropertyProjection(
        scriptSerialized,
        scriptTargets,
        {});
    const auto scriptAssetProperty = std::find_if(csharpProperties.begin(), csharpProperties.end(), [](const PropertyNode& node)
    {
        return node.fieldName == "scriptAsset";
    });
    ASSERT_NE(scriptAssetProperty, csharpProperties.end());
    EXPECT_TRUE(scriptAssetProperty->readOnly);
    const auto scriptAssetValue = scriptSerialized.FindProperty("scriptAsset").GetValue();
    ASSERT_TRUE(scriptAssetValue.IsValid());
    EXPECT_EQ(scriptAssetValue.GetValue<std::string>(), csharpAsset.sourcePath);
    const auto angleProperty = std::find_if(csharpProperties.begin(), csharpProperties.end(), [](const PropertyNode& node)
    {
        return node.fieldName == "angle";
    });
    ASSERT_NE(angleProperty, csharpProperties.end());
    EXPECT_EQ(angleProperty->displayName, "Angle");
    EXPECT_EQ(angleProperty->kind, InspectorPropertyKind::Float);
    const auto angleValue = scriptSerialized.FindProperty("angle").GetValue();
    ASSERT_TRUE(angleValue.IsValid());
    EXPECT_FLOAT_EQ(angleValue.GetValue<float>(), 90.0f);

    Scripting::ScriptAsset luaAsset;
    luaAsset.language = Scripting::ScriptLanguage::Lua;
    luaAsset.sourcePath = "Assets/Scripts/FlyController.lua";
    luaAsset.scriptType = flyController.id;
    luaAsset.isComponent = true;
    script->SetScriptAsset(luaAsset);
    ASSERT_TRUE(script->SetSerializedField(speed.id, int32_t {12}));

    EXPECT_NE(adapter.GetStructureVersion(targets), csharpVersion);
    const auto luaPresentation = adapter.BuildPresentation(targets, {});
    const auto* luaNode = findScriptNode(luaPresentation);
    ASSERT_NE(luaNode, nullptr);
    EXPECT_EQ(luaNode->displayName, "Fly Controller (Lua)");
    EXPECT_EQ(std::find_if(luaNode->children.begin(), luaNode->children.end(), [](const PropertyNode& node)
    {
        return node.fieldName == "speed" || node.fieldName == "angle";
    }), luaNode->children.end());

    ASSERT_TRUE(scriptSerialized.UpdateIfRequiredOrScript());
    const auto luaProperties = InspectorProjectionBuilder::BuildPropertyProjection(
        scriptSerialized,
        scriptTargets,
        {});
    EXPECT_NE(std::find_if(luaProperties.begin(), luaProperties.end(), [](const PropertyNode& node)
    {
        return node.fieldName == "speed";
    }), luaProperties.end());
    EXPECT_EQ(std::find_if(luaProperties.begin(), luaProperties.end(), [](const PropertyNode& node)
    {
        return node.fieldName == "angle";
    }), luaProperties.end());
}

TEST(InspectorCoreTests, AccessorBackendSupportsScriptObjectReferencesAndRejectsMismatchedSchemas)
{
    Scripting::ScriptApiDatabase api;
    Scripting::ScriptTypeDescriptor partiallySupported;
    partiallySupported.id = 310u;
    partiallySupported.name = "Gameplay.PartiallySupported";
    Scripting::ScriptFieldDescriptor speed;
    speed.id = 3101u;
    speed.name = "speed";
    speed.type = {3102u, Scripting::ScriptValueKind::Float, "float", sizeof(float), alignof(float), true};
    partiallySupported.fields.push_back(speed);
    Scripting::ScriptFieldDescriptor target;
    target.id = 3103u;
    target.name = "target";
    target.type = {3104u, Scripting::ScriptValueKind::ObjectReference, "GameObject", 0u, 0u, true};
    partiallySupported.fields.push_back(target);
    ASSERT_TRUE(api.RegisterType(partiallySupported));

    Scripting::ScriptTypeDescriptor otherShape;
    otherShape.id = 320u;
    otherShape.name = "Gameplay.OtherShape";
    Scripting::ScriptFieldDescriptor count;
    count.id = 3201u;
    count.name = "count";
    count.type = {3202u, Scripting::ScriptValueKind::Int32, "int", sizeof(int32_t), alignof(int32_t), true};
    otherShape.fields.push_back(count);
    ASSERT_TRUE(api.RegisterType(otherShape));

    Scripting::ScriptRuntime runtime;
    ASSERT_TRUE(runtime.Initialize(api).Succeeded());
    Engine::GameObject firstObject("First Script");
    Engine::GameObject secondObject("Second Script");
    Engine::GameObject referenceObject("Script Reference");
    auto* first = firstObject.AddComponent<Scripting::ScriptComponent>();
    auto* second = secondObject.AddComponent<Scripting::ScriptComponent>();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    first->SetRuntime(&runtime);
    second->SetRuntime(&runtime);

    Scripting::ScriptAsset partialAsset;
    partialAsset.language = Scripting::ScriptLanguage::CSharp;
    partialAsset.scriptType = partiallySupported.id;
    partialAsset.sourcePath = "Assets/Scripts/PartiallySupported.cs";
    first->SetScriptAsset(partialAsset);
    ASSERT_TRUE(first->SetSerializedField(
        target.id,
        Scripting::NativeObjectHandle::FromInstanceId(referenceObject.GetInstanceID())));

    Serialization::SerializedTargetSet partialTargets;
    partialTargets.targets = {InspectorTargetHandle::FromObject(*first, InspectorTargetKind::NativeObject)};
    partialTargets.Normalize();
    // ObjectReference script fields use the same byte-stream/reference-table
    // path as native fields. Their presence must not route the component back
    // through a removed Inspector binding bridge.
    ASSERT_TRUE(Serialization::AccessorSerializedBackend::Supports(partialTargets));
    Serialization::SerializedObject partialSerialized(
        partialTargets,
        Serialization::AccessorSerializedBackend::Create());
    const auto serializedTarget = partialSerialized.FindProperty("target");
    ASSERT_TRUE(serializedTarget);
    ASSERT_EQ(serializedTarget.GetPropertyType(), Serialization::SerializedPropertyType::ObjectReference);
    ASSERT_TRUE(serializedTarget.GetValue().IsValid());
    EXPECT_EQ(
        serializedTarget.GetValue().GetValue<Engine::Serialize::PPtr<Engine::GameObject>>().GetInstanceID(),
        referenceObject.GetInstanceID());
    ASSERT_TRUE(serializedTarget.SetValue(meta::Variant(
        Engine::Serialize::PPtr<Engine::GameObject>(secondObject.GetInstanceID()))));
    ASSERT_TRUE(partialSerialized.ApplyModifiedPropertiesWithoutUndo());
    const auto* updatedTarget = first->FindSerializedField(target.id);
    ASSERT_NE(updatedTarget, nullptr);
    ASSERT_TRUE(std::holds_alternative<Scripting::NativeObjectHandle>(*updatedTarget));
    EXPECT_EQ(
        std::get<Scripting::NativeObjectHandle>(*updatedTarget).GetInstanceId(),
        secondObject.GetInstanceID());

    Scripting::ScriptAsset otherAsset;
    otherAsset.language = Scripting::ScriptLanguage::CSharp;
    otherAsset.scriptType = otherShape.id;
    otherAsset.sourcePath = "Assets/Scripts/OtherShape.cs";
    second->SetScriptAsset(otherAsset);

    Serialization::SerializedTargetSet mixedTargets;
    mixedTargets.targets = {
        InspectorTargetHandle::FromObject(*first, InspectorTargetKind::NativeObject),
        InspectorTargetHandle::FromObject(*second, InspectorTargetKind::NativeObject)};
    mixedTargets.Normalize();
    EXPECT_FALSE(Serialization::AccessorSerializedBackend::Supports(mixedTargets));
}

TEST(InspectorCoreTests, ScriptComponentTitleUsesDeclarationWhenRuntimeDescriptorIsUnavailable)
{
    Engine::GameObject object("Script Source Title");
    auto* script = object.AddComponent<Scripting::ScriptComponent>();
    ASSERT_NE(script, nullptr);

    Scripting::ScriptAsset asset;
    asset.language = Scripting::ScriptLanguage::CSharp;
    asset.sourceText = R"(
        // class NotTheComponent : Behaviour {}
        public sealed class PlayerController : Behaviour
        {
            private const string text = "class AlsoNotTheComponent : Behaviour";
        }
    )";
    script->SetScriptAsset(asset);

    EXPECT_EQ(
        ScriptSerializedAdapter::GetComponentDisplayName(*script),
        "Player Controller (C#)");
}

TEST(InspectorCoreTests, GameObjectHeaderAndComponentEnabledUseCanonicalSerializedProperties)
{
    ScopedInspectorUndoService undo;
    Engine::GameObject object("Canonical Header Target");
    auto* camera = object.AddComponent<Engine::Components::CameraComponent>();
    ASSERT_NE(camera, nullptr);

    InspectorTargetSet targets;
    targets.targets = {InspectorTargetHandle::FromObject(object, InspectorTargetKind::GameObject)};
    const auto presentation = NativeReflectionAdapter().BuildPresentation(targets, {});
    ASSERT_EQ(presentation.roots.size(), 1u);
    EXPECT_TRUE(presentation.auxiliary.empty());

    Serialization::SerializedObject serialized(
        targets,
        Serialization::AccessorSerializedBackend::Create());
    const auto active = serialized.FindProperty("GameObject.active");
    const auto name = serialized.FindProperty("GameObject.name");
    const auto tag = serialized.FindProperty("GameObject.tag");
    const auto layer = serialized.FindProperty("GameObject.layer");
    ASSERT_TRUE(active);
    ASSERT_TRUE(name);
    ASSERT_TRUE(tag);
    ASSERT_TRUE(layer);
    EXPECT_TRUE(active.GetBoolValue());
    EXPECT_EQ(name.GetStringValue(), "Canonical Header Target");
    EXPECT_EQ(tag.GetStringValue(), "Untagged");
    EXPECT_EQ(layer.GetLongValue(), 0);

    const auto cameraNode = std::find_if(
        presentation.roots.front().children.begin(),
        presentation.roots.front().children.end(),
        [](const PropertyNode& node)
        {
            return node.valueType == NLS_TYPEOF(Engine::Components::CameraComponent);
        });
    ASSERT_NE(cameraNode, presentation.roots.front().children.end());
    Serialization::SerializedTargetSet componentTargets;
    componentTargets.targets = {
        InspectorTargetHandle::FromObject(*camera, InspectorTargetKind::NativeObject)};
    Serialization::SerializedObject componentHeader(
        componentTargets,
        Serialization::AccessorSerializedBackend::CreateComponentHeader());
    const auto enabled = componentHeader.FindProperty("enabled");
    ASSERT_TRUE(enabled);
    EXPECT_TRUE(enabled.GetBoolValue());

    ASSERT_TRUE(active.SetBoolValue(false));
    ASSERT_TRUE(enabled.SetBoolValue(false));
    EXPECT_TRUE(object.IsSelfActive());
    EXPECT_TRUE(camera->IsSelfEnabled());
    ASSERT_TRUE(serialized.ApplyModifiedProperties());
    ASSERT_TRUE(componentHeader.ApplyModifiedProperties());
    EXPECT_FALSE(object.IsSelfActive());
    EXPECT_FALSE(camera->IsSelfEnabled());

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_TRUE(camera->IsSelfEnabled());
    EXPECT_FALSE(object.IsSelfActive());
    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_TRUE(object.IsSelfActive());
    EXPECT_TRUE(camera->IsSelfEnabled());
    ASSERT_TRUE(undo.Get().RequestRedo().Succeeded());
    EXPECT_FALSE(object.IsSelfActive());
    EXPECT_TRUE(camera->IsSelfEnabled());
    ASSERT_TRUE(undo.Get().RequestRedo().Succeeded());
    EXPECT_FALSE(object.IsSelfActive());
    EXPECT_FALSE(camera->IsSelfEnabled());
}

TEST(InspectorCoreTests, GeneratedCanvasUsesTypedDocumentReferenceWithoutHiddenMetadata)
{
    const auto canvasType = NLS_TYPEOF(Engine::Components::UICanvasComponent);
    const auto cameraId = canvasType.GetField("cameraId");
    const auto renderModeValue = canvasType.GetField("renderModeValue");
    const auto document = canvasType.GetField("document");
    ASSERT_TRUE(cameraId.IsValid());
    ASSERT_TRUE(renderModeValue.IsValid());
    ASSERT_TRUE(document.IsValid());
    EXPECT_EQ(GetInspectorPropertyKind(cameraId.GetType()), InspectorPropertyKind::UnsignedInteger64);
    EXPECT_EQ(GetInspectorPropertyKind(renderModeValue.GetType()), InspectorPropertyKind::UnsignedInteger32);
    EXPECT_EQ(GetInspectorPropertyKind(document.GetType()), InspectorPropertyKind::ObjectReference);
    EXPECT_FALSE(BuildEditorPropertyMetadata(canvasType.GetName(), "document").hideInInspector);
    EXPECT_FALSE(BuildEditorPropertyMetadata(canvasType.GetName(), "cameraId").hideInInspector);
}

TEST(InspectorCoreTests, ComponentEditorPropertyRowsUseSharedSearchMatching)
{
    EXPECT_TRUE(ComponentEditorContext::MatchesSearch("Document", "document", "document", "document"));
    EXPECT_TRUE(ComponentEditorContext::MatchesSearch("Sort Layer", "sortLayer", "sortLayer", "LAYER"));
    EXPECT_TRUE(ComponentEditorContext::MatchesSearch({}, "cameraId", "cameraId", "camera"));
    EXPECT_TRUE(ComponentEditorContext::MatchesSearch("Render Mode", "renderMode", "renderMode", {}));
    EXPECT_FALSE(ComponentEditorContext::MatchesSearch("Document", "document", "document", "camera"));
    EXPECT_FALSE(ComponentEditorContext::MatchesSearch("Sort Order", "sortOrder", "sortOrder", "layer"));
}

TEST(InspectorCoreTests, LayoutKeepsLabelsAndActionsInsideTheirAllocatedRectsAtAllSupportedDpis)
{
    constexpr std::array<float, 3u> logicalWidths {320.0f, 480.0f, 840.0f};
    constexpr std::array<float, 3u> scales {1.0f, 1.5f, 2.0f};
    constexpr std::array<float, 3u> actionLabelLogicalWidths {34.0f, 43.0f, 72.0f};

    for (const float logicalWidth : logicalWidths)
    {
        for (const float scale : scales)
        {
            const auto properties = BuildInspectorPropertiesLayout(logicalWidth * scale, scale);
            EXPECT_EQ(properties.compact, logicalWidth < kInspectorCompactLogicalWidth);
            if (!properties.compact)
            {
                EXPECT_GE(properties.labelLogicalWidth, kInspectorMinimumLabelLogicalWidth);
                EXPECT_LE(properties.labelLogicalWidth, kInspectorMaximumLabelLogicalWidth);
                EXPECT_GE(properties.valueLogicalWidth, 0.0f);
                EXPECT_LE(properties.labelLogicalWidth + properties.valueLogicalWidth, logicalWidth + 0.001f);
            }

            std::vector<float> actionLabelWidths;
            actionLabelWidths.reserve(actionLabelLogicalWidths.size());
            for (const float width : actionLabelLogicalWidths)
                actionLabelWidths.push_back(width * scale);
            const auto actions = BuildInspectorActionLayout(
                logicalWidth * scale,
                scale,
                actionLabelWidths,
                8.0f * scale,
                16.0f * scale);
            ASSERT_EQ(actions.buttons.size(), actionLabelWidths.size());
            EXPECT_EQ(actions.stacked, logicalWidth < kInspectorCompactLogicalWidth);
            for (size_t index = 0u; index < actions.buttons.size(); ++index)
            {
                const auto& button = actions.buttons[index];
                EXPECT_GE(button.width, actionLabelWidths[index] + 16.0f * scale);
                EXPECT_GE(button.x, 0.0f);
                EXPECT_LE(button.x + button.width, logicalWidth * scale + 0.001f);
                for (size_t other = index + 1u; other < actions.buttons.size(); ++other)
                {
                    const auto& next = actions.buttons[other];
                    const bool disjointX = button.x + button.width <= next.x ||
                        next.x + next.width <= button.x;
                    const bool disjointY = button.y + button.height <= next.y ||
                        next.y + next.height <= button.y;
                    EXPECT_TRUE(disjointX || disjointY);
                }
            }
        }
    }
}

TEST(InspectorCoreTests, MultiInputLayoutKeepsComponentLabelsAndValuesInsideTheirAllocatedRects)
{
    constexpr std::array<float, 4u> logicalWidths {96.0f, 320.0f, 480.0f, 840.0f};
    constexpr std::array<float, 3u> scales {1.0f, 1.5f, 2.0f};
    constexpr std::array<size_t, 3u> componentCounts {2u, 3u, 4u};

    for (const float logicalWidth : logicalWidths)
    {
        for (const float scale : scales)
        {
            const float availableWidth = logicalWidth * scale;
            const float spacing = 8.0f * scale;
            const float componentLabelWidth = 20.0f * scale;
            for (const size_t componentCount : componentCounts)
            {
                const auto layout = BuildInspectorMultiFieldLayout(
                    availableWidth,
                    scale,
                    componentCount,
                    spacing,
                    componentLabelWidth);

                EXPECT_GT(layout.groupWidth, 0.0f);
                EXPECT_GT(layout.inputWidth, 0.0f);
                EXPECT_LE(layout.inputWidth + componentLabelWidth, layout.groupWidth + 0.001f);
                if (layout.stacked)
                {
                    EXPECT_FLOAT_EQ(layout.groupWidth, availableWidth);
                }
                else
                {
                    EXPECT_GE(layout.inputWidth, kInspectorMinimumComponentInputLogicalWidth * scale);
                    EXPECT_LE(
                        layout.groupWidth * static_cast<float>(componentCount) +
                            spacing * static_cast<float>(componentCount - 1u),
                        availableWidth + 0.001f);
                }
            }
        }
    }
}

TEST(InspectorCoreTests, ComponentHeaderClosesItsTableBeforeLeavingTheFoldoutScope)
{
    ImGuiContext* context = ImGui::CreateContext();
    ASSERT_NE(context, nullptr);

    ImGui::GetIO().DisplaySize = ImVec2(800.0f, 600.0f);
    unsigned char* fontPixels = nullptr;
    int fontWidth = 0;
    int fontHeight = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    ASSERT_NE(fontPixels, nullptr);
    ASSERT_GT(fontWidth, 0);
    ASSERT_GT(fontHeight, 0);
    ImGui::NewFrame();
    {
        Engine::GameObject gameObject("Inspector table scope");
        InspectorTargetSet targets;
        targets.targets.push_back(InspectorTargetHandle::FromObject(
            gameObject,
            InspectorTargetKind::GameObject));

        InspectorView view;
        view.SetToolbarVisible(false);
        view.SetTargets(std::move(targets));

        ImGui::Begin("InspectorTableScopeTest");
        view.Draw();
        ImGui::End();
    }
    ImGui::Render();
    ImGui::DestroyContext(context);
}

TEST(InspectorCoreTests, TransformAccessorIteratorUsesOnlyCanonicalStreamFields)
{
    Engine::GameObject gameObject("Transform stream iterator");
    auto* transform = gameObject.GetTransform();
    ASSERT_NE(transform, nullptr);

    Serialization::SerializedTargetSet targets;
    targets.targets = {InspectorTargetHandle::FromObject(*transform, InspectorTargetKind::NativeObject)};
    targets.Normalize();
    ASSERT_TRUE(Serialization::AccessorSerializedBackend::Supports(targets));

    Serialization::SerializedObject serialized(
        targets,
        Serialization::AccessorSerializedBackend::Create());
    ASSERT_TRUE(serialized.IsValid()) << serialized.GetLastError();

    auto iterator = serialized.GetIterator();
    ASSERT_TRUE(iterator);
    std::vector<std::string> paths;
    bool reachedEnd = false;
    for (size_t step = 0u; step < 8u; ++step)
    {
        if (!iterator.NextVisible(true))
        {
            reachedEnd = true;
            break;
        }
        paths.emplace_back(iterator.GetPropertyPath());
    }

    // Vector3 and Quaternion have fixed stream layouts. Their X/Y/Z/W
    // controls belong to their drawers, so reflection must not add duplicate
    // child fields that make textual iterator paths cycle indefinitely.
    EXPECT_TRUE(reachedEnd);
    EXPECT_EQ(paths, (std::vector<std::string> {
        "localPosition", "localRotation", "localScale"}));
}

TEST(InspectorCoreTests, ComponentEditorsAreExecutableRegistryEntries)
{
    auto& registry = InspectorRegistry::Get();
    registry.RegisterComponentEditor({
        NLS_TYPEOF(Engine::Components::LightComponent),
        "InspectorCoreLightComponentEditor",
        false,
        false,
        false,
        []
        {
            return std::make_unique<DefaultComponentEditor>();
        }});

    const auto editor = registry.ResolveComponentEditor(NLS_TYPEOF(Engine::Components::LightComponent));
    ASSERT_TRUE(editor.has_value());
    EXPECT_EQ(editor->identifier, "InspectorCoreLightComponentEditor");
    EXPECT_TRUE(static_cast<bool>(editor->create));
    EXPECT_NE(editor->create(), nullptr);
}

TEST(InspectorCoreTests, NativeComponentEditorsAreDiscoveredWithoutCanvasStartupRegistration)
{
    InspectorRegistry registry;
    NLS_META_GENERATED_LINK_FUNCTION();
    (void)meta::ReflectionDatabase::Instance();
    NLS_META_GENERATED_REGISTER_EDITOR_EXTENSIONS(registry);

    const auto editor = registry.ResolveComponentEditor(NLS_TYPEOF(Engine::Components::UICanvasComponent));
    ASSERT_TRUE(editor.has_value());
    EXPECT_EQ(editor->identifier, "UICanvasComponentEditor");
    ASSERT_TRUE(static_cast<bool>(editor->create));
    EXPECT_NE(editor->create(), nullptr);
}

TEST(InspectorCoreTests, GeneratedComponentEditorDescriptorDuplicatesKeepRegistryValidation)
{
    InspectorRegistry registry;
    (void)meta::ReflectionDatabase::Instance();
    const EditorExtensionDescriptor first {
        EditorExtensionKind::ComponentEditor,
        NLS_TYPEOF(Engine::Components::LightComponent).GetKey(),
        "NLS::Engine::Components::LightComponent",
        NLS_TYPEOF(UICanvasComponentEditor).GetKey(),
        "NLS::Editor::Inspector::UICanvasComponentEditor",
        "InspectorCoreDuplicateNativeEditor",
        false,
        false,
        false,
        0,
        []() -> std::unique_ptr<ComponentEditor> { return std::make_unique<DefaultComponentEditor>(); }};
    const EditorExtensionDescriptor duplicate {
        EditorExtensionKind::ComponentEditor,
        NLS_TYPEOF(Engine::Components::LightComponent).GetKey(),
        "NLS::Engine::Components::LightComponent",
        NLS_TYPEOF(UICanvasComponentEditor).GetKey(),
        "NLS::Editor::Inspector::UICanvasComponentEditor",
        "InspectorCoreDuplicateNativeEditorSecondDeclaration",
        false,
        true,
        false,
        0,
        []() -> std::unique_ptr<ComponentEditor> { return std::make_unique<DefaultComponentEditor>(); }};
    const std::array descriptors {first, duplicate};

    EXPECT_THROW(
        RegisterEditorExtensionDescriptors(registry, descriptors),
        std::logic_error);
}

TEST(InspectorCoreTests, ComponentEditorPrioritySelectsDeterministicallyWithinResolutionPhase)
{
    InspectorRegistry registry;
    registry.RegisterComponentEditor({
        NLS_TYPEOF(Engine::Components::LightComponent),
        "InspectorCoreLowPriorityEditor",
        false,
        false,
        false,
        [] { return std::make_unique<DefaultComponentEditor>(); },
        -10});
    registry.RegisterComponentEditor({
        NLS_TYPEOF(Engine::Components::LightComponent),
        "InspectorCoreHighPriorityEditor",
        false,
        false,
        false,
        [] { return std::make_unique<DefaultComponentEditor>(); },
        25});

    const auto editor = registry.ResolveComponentEditor(NLS_TYPEOF(Engine::Components::LightComponent));
    ASSERT_TRUE(editor.has_value());
    EXPECT_EQ(editor->identifier, "InspectorCoreHighPriorityEditor");
}

TEST(InspectorCoreTests, PropertyAndDecoratorDrawersAreExecutableRegistryEntries)
{
    struct CallbackState
    {
        bool propertyCalled = false;
        bool decoratorCalled = false;
    };
    const auto state = std::make_shared<CallbackState>();
    const PropertyPath fieldPath("InspectorCoreTests.ExecutableDrawer.value");
    auto& registry = InspectorRegistry::Get();
    registry.RegisterPropertyDrawer({
        InspectorDrawerScope::Field,
        "InspectorCoreExecutablePropertyDrawer",
        fieldPath,
        {},
        {},
        [state](InspectorPropertyDrawerContext& context)
        {
            state->propertyCalled = true;
            return context.preview && context.preview(meta::Variant(int32_t {27}), "Custom property drawer edit");
        }});
    registry.RegisterDecoratorDrawer({
        "Header",
        "InspectorCoreExecutableDecoratorDrawer",
        [state](const InspectorDecoratorDrawerContext&)
        {
            state->decoratorCalled = true;
        }});

    PropertyNode node;
    node.path = fieldPath;
    node.valueType = NLS_TYPEOF(int32_t);
    node.metadata.header = "Drawer Header";
    auto registration = registry.ResolvePropertyDrawer(node);
    ASSERT_TRUE(registration.has_value());
    ASSERT_TRUE(static_cast<bool>(registration->draw));

    auto value = meta::Variant(int32_t {4});
    std::string widgetId("##InspectorCoreDrawer");
    InspectorPropertyDrawerContext context {node, value, widgetId};
    context.preview = [&context, &value](meta::Variant updated, std::string)
    {
        value = std::move(updated);
        context.changed = true;
        return true;
    };
    EXPECT_TRUE(registration->draw(context));
    EXPECT_TRUE(state->propertyCalled);
    EXPECT_TRUE(context.changed);
    EXPECT_EQ(value.GetValue<int32_t>(), 27);

    const auto decorators = registry.ResolveDecoratorDrawers(node);
    ASSERT_EQ(decorators.size(), 1u);
    ASSERT_TRUE(static_cast<bool>(decorators.front().draw));
    decorators.front().draw({node, 1.0f});
    EXPECT_TRUE(state->decoratorCalled);
}

TEST(InspectorCoreTests, MultiTargetComponentsIntersectByTypeAndOccurrenceInsteadOfArrayIndex)
{
    Engine::GameObject first("First Inspector Target");
    Engine::GameObject second("Second Inspector Target");
    ASSERT_NE(first.AddComponent<Engine::Components::LightComponent>(), nullptr);
    ASSERT_NE(first.AddComponent<Engine::Components::CameraComponent>(), nullptr);
    ASSERT_NE(second.AddComponent<Engine::Components::CameraComponent>(), nullptr);
    ASSERT_NE(second.AddComponent<Engine::Components::LightComponent>(), nullptr);

    InspectorTargetSet targets;
    targets.targets = {
        InspectorTargetHandle::FromObject(first, InspectorTargetKind::GameObject),
        InspectorTargetHandle::FromObject(second, InspectorTargetKind::GameObject)};
    targets.Normalize();

    const auto presentation = NativeReflectionAdapter().BuildPresentation(targets, {});
    ASSERT_EQ(presentation.roots.size(), 1u);
    ASSERT_EQ(presentation.roots.front().children.size(), 3u);
    for (const auto& component : presentation.roots.front().children)
    {
        const auto componentTargets = Inspector::GetPropertyNodeTargets(component);
        ASSERT_EQ(componentTargets.size(), 2u);
        const auto* firstComponent = NativeReflectionAdapter::ResolveObject(componentTargets[0]);
        const auto* secondComponent = NativeReflectionAdapter::ResolveObject(componentTargets[1]);
        ASSERT_NE(firstComponent, nullptr);
        ASSERT_NE(secondComponent, nullptr);
        EXPECT_EQ(firstComponent->GetType().GetName(), secondComponent->GetType().GetName());
    }
}

TEST(InspectorCoreTests, ObjectTargetsResolveThroughPersistentIdentityWhenTheInstanceCacheChanges)
{
    Engine::GameObject object("Persistent Inspector Target");
    auto handle = InspectorTargetHandle::FromObject(object, InspectorTargetKind::GameObject);
    ASSERT_TRUE(handle.objectIdentifier.IsValid());
    ASSERT_NE(handle.instanceId, InstanceID_None);

    // InstanceID is intentionally not an Undo identity. A rebuilt object can
    // occupy a different runtime slot while the selected persistent object
    // remains resolvable through its ObjectIdentifier.
    handle.instanceId = InstanceID_None;
    EXPECT_EQ(NativeReflectionAdapter::ResolveObject(handle), &object);

    auto sameTargetWithDifferentCache = handle;
    sameTargetWithDifferentCache.instanceId = object.GetInstanceID();
    InspectorTargetSet targets;
    targets.targets = {handle, sameTargetWithDifferentCache};
    targets.Normalize();
    EXPECT_EQ(targets.targets.size(), 1u);
}

TEST(InspectorCoreTests, CanvasCustomPropertiesUseCanonicalAccessorByteStream)
{
    Engine::GameObject object("Canvas Holder");
    auto* canvas = object.AddComponent<Engine::Components::UICanvasComponent>();
    ASSERT_NE(canvas, nullptr);

    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(*canvas, InspectorTargetKind::NativeObject));
    targets.Normalize();
    ASSERT_TRUE(Serialization::AccessorSerializedBackend::Supports(targets));

    Serialization::SerializedObject serialized(targets, Serialization::AccessorSerializedBackend::Create());
    ASSERT_TRUE(serialized.IsValid());
    const auto sortOrder = serialized.FindProperty("sortOrderScript");
    const auto document = serialized.FindProperty("document");
    ASSERT_TRUE(sortOrder);
    ASSERT_TRUE(document);
    EXPECT_EQ(sortOrder.GetValue().GetValue<int32_t>(), 0);

    ASSERT_TRUE(sortOrder.SetValue(meta::Variant(int32_t {42})));
    const auto documentId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("2a9d7d1a-e258-4a4f-99e5-e65cbdb44e11"));
    ASSERT_TRUE(document.SetValue(meta::Variant(Engine::UI::UIDocumentReference(documentId, "Assets/Canvas.ui"))));
    ASSERT_TRUE(serialized.ApplyModifiedPropertiesWithoutUndo());
    EXPECT_EQ(canvas->GetSortOrder(), 42);
    EXPECT_EQ(canvas->GetDocumentPath(), "Assets/Canvas.ui");
    EXPECT_EQ(canvas->GetDocumentReference().GetAssetId(), documentId);
}

TEST(InspectorCoreTests, GameObjectHeaderUsesCanonicalAccessorByteStream)
{
    Engine::GameObject object("Before");
    object.SetTag("Untagged");
    object.SetLayer(0);

    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(object, InspectorTargetKind::GameObject));
    targets.Normalize();
    ASSERT_TRUE(Serialization::AccessorSerializedBackend::Supports(targets));

    Serialization::SerializedObject serialized(targets, Serialization::AccessorSerializedBackend::Create());
    ASSERT_TRUE(serialized.IsValid());
    const auto active = serialized.FindProperty("GameObject.active");
    const auto name = serialized.FindProperty("GameObject.name");
    const auto tag = serialized.FindProperty("GameObject.tag");
    const auto layer = serialized.FindProperty("GameObject.layer");
    ASSERT_TRUE(active);
    ASSERT_TRUE(name);
    ASSERT_TRUE(tag);
    ASSERT_TRUE(layer);
    EXPECT_TRUE(active.GetBoolValue());
    EXPECT_EQ(name.GetStringValue(), "Before");

    ASSERT_TRUE(active.SetBoolValue(false));
    ASSERT_TRUE(name.SetStringValue("After"));
    ASSERT_TRUE(tag.SetStringValue("Player"));
    ASSERT_TRUE(layer.SetLongValue(9));
    ASSERT_TRUE(serialized.ApplyModifiedPropertiesWithoutUndo());
    EXPECT_FALSE(object.IsSelfActive());
    EXPECT_EQ(object.GetName(), "After");
    EXPECT_EQ(object.GetTag(), "Player");
    EXPECT_EQ(object.GetLayer(), 9);
}

TEST(InspectorCoreTests, ComponentEnabledHeaderUsesCanonicalAccessorByteStream)
{
    ScopedInspectorUndoService undo;
    Engine::GameObject object("Component Header");
    auto* camera = object.AddComponent<Engine::Components::CameraComponent>();
    ASSERT_NE(camera, nullptr);

    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(*camera, InspectorTargetKind::NativeObject));
    targets.Normalize();
    ASSERT_TRUE(Serialization::AccessorSerializedBackend::SupportsComponentHeader(targets));

    Serialization::SerializedObject serialized(
        targets,
        Serialization::AccessorSerializedBackend::CreateComponentHeader());
    const auto enabled = serialized.FindProperty("enabled");
    ASSERT_TRUE(enabled);
    EXPECT_TRUE(enabled.GetBoolValue());
    ASSERT_TRUE(enabled.SetBoolValue(false));
    ASSERT_TRUE(serialized.ApplyModifiedProperties());
    EXPECT_FALSE(camera->IsSelfEnabled());
    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_TRUE(camera->IsSelfEnabled());
    ASSERT_TRUE(undo.Get().RequestRedo().Succeeded());
    EXPECT_FALSE(camera->IsSelfEnabled());

    auto* transform = object.GetTransform();
    ASSERT_NE(transform, nullptr);
    Serialization::SerializedTargetSet transformTargets;
    transformTargets.targets.push_back(InspectorTargetHandle::FromObject(*transform, InspectorTargetKind::NativeObject));
    transformTargets.Normalize();
    Serialization::SerializedObject transformSerialized(
        transformTargets,
        Serialization::AccessorSerializedBackend::CreateComponentHeader());
    const auto transformEnabled = transformSerialized.FindProperty("enabled");
    ASSERT_TRUE(transformEnabled);
    EXPECT_FALSE(transformEnabled.IsEditable());
    EXPECT_FALSE(transformEnabled.SetBoolValue(false));
}

TEST(InspectorCoreTests, TransformLocalPositionStreamEditAppliesThroughUndo)
{
    ScopedInspectorUndoService undoScope;
    Engine::GameObject object("Transform stream edit");
    auto* transform = object.GetTransform();
    ASSERT_NE(transform, nullptr);

    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(*transform, InspectorTargetKind::NativeObject));
    targets.Normalize();
    ASSERT_TRUE(Serialization::AccessorSerializedBackend::Supports(targets));

    Serialization::SerializedObject serialized(
        targets,
        Serialization::AccessorSerializedBackend::Create());
    ASSERT_TRUE(serialized.IsValid()) << serialized.GetLastError();

    const auto localPosition = serialized.FindProperty("localPosition");
    ASSERT_TRUE(localPosition);
    EXPECT_TRUE(localPosition.IsEditable());

    ASSERT_TRUE(localPosition.SetValue(meta::Variant(NLS::Maths::Vector3 {1.0f, 2.0f, 3.0f})));
    EXPECT_TRUE(serialized.HasModifiedProperties());
    ASSERT_TRUE(serialized.ApplyModifiedProperties()) << serialized.GetLastError();

    const auto applied = transform->GetLocalPosition();
    EXPECT_FLOAT_EQ(applied.x, 1.0f);
    EXPECT_FLOAT_EQ(applied.y, 2.0f);
    EXPECT_FLOAT_EQ(applied.z, 3.0f);

    ASSERT_TRUE(undoScope.Get().RequestUndo().Succeeded());
    const auto restored = transform->GetLocalPosition();
    EXPECT_FLOAT_EQ(restored.x, 0.0f);
    EXPECT_FLOAT_EQ(restored.y, 0.0f);
    EXPECT_FLOAT_EQ(restored.z, 0.0f);
    ASSERT_TRUE(undoScope.Get().RequestRedo().Succeeded());
    const auto reapplied = transform->GetLocalPosition();
    EXPECT_FLOAT_EQ(reapplied.x, 1.0f);
    EXPECT_FLOAT_EQ(reapplied.y, 2.0f);
    EXPECT_FLOAT_EQ(reapplied.z, 3.0f);
}

TEST(InspectorCoreTests, ComponentEditorKeepsStagedPreviewAcrossPerFrameRebind)
{
    ScopedInspectorUndoService undoScope;
    Engine::GameObject object("Staged preview");
    auto* transform = object.GetTransform();
    ASSERT_NE(transform, nullptr);

    PropertyNode componentNode;
    componentNode.path = PropertyPath("GameObject.Components.TransformComponent[0]");
    componentNode.target = InspectorTargetHandle::FromObject(*transform, InspectorTargetKind::NativeObject);
    SetPropertyNodeTargets(componentNode, {componentNode.target});
    componentNode.isComponent = true;
    componentNode.isTransform = true;

    Inspector::DefaultComponentEditor editor;
    editor.UpdateSerializedObject(componentNode);
    auto& serialized = editor.GetSerializedObject();
    ASSERT_TRUE(serialized.IsValid()) << serialized.GetLastError();

    const auto localPosition = serialized.FindProperty("localPosition");
    ASSERT_TRUE(localPosition);
    // A drag stages its value on the frame that reports the change.
    ASSERT_TRUE(localPosition.SetValue(meta::Variant(NLS::Maths::Vector3 {4.0f, 5.0f, 6.0f})));

    // The Inspector rebinds component editors every frame before drawing. That
    // rebind must not discard the staged drag preview.
    editor.UpdateSerializedObject(componentNode);
    EXPECT_TRUE(serialized.HasModifiedProperties());
    const auto staged = serialized.FindProperty("localPosition");
    ASSERT_TRUE(staged);
    const auto stagedValue = staged.GetValue();
    ASSERT_TRUE(stagedValue.IsValid());
    EXPECT_FLOAT_EQ(stagedValue.GetValue<NLS::Maths::Vector3>().x, 4.0f);

    // Releasing the drag commits the staged preview through the undo boundary.
    ASSERT_TRUE(serialized.ApplyModifiedProperties()) << serialized.GetLastError();
    const auto applied = transform->GetLocalPosition();
    EXPECT_FLOAT_EQ(applied.x, 4.0f);
    EXPECT_FLOAT_EQ(applied.y, 5.0f);
    EXPECT_FLOAT_EQ(applied.z, 6.0f);
}

TEST(InspectorCoreTests, ImporterReimportCompletionRejectsStaleRevisionWithoutChangingDraft)
{
    const auto target = InspectorTargetHandle::FromAsset(
        InspectorTargetKind::AssetImporter,
        "Assets/Models/StaleCompletion.fbx",
        "e5fcb4c1-b29d-4ea6-9d26-2b650b3e72f4");

    const auto state = TestHooks::CompleteAssetImporterReimportForTesting(
        target,
        7u,
        6u,
        {{"MODEL_SCALE", "1.0"}},
        std::map<std::string, std::string> {{"MODEL_SCALE", "2.0"}},
        true,
        {});

    EXPECT_TRUE(state.available);
    EXPECT_FALSE(state.dirty);
    EXPECT_TRUE(state.reimportQueued);
    EXPECT_EQ(state.revision, 7u);
    ASSERT_EQ(state.settings.size(), 1u);
    EXPECT_EQ(state.settings.at("MODEL_SCALE"), "1.0");
    EXPECT_TRUE(state.lastError.empty());
}

TEST(InspectorCoreTests, ImporterReimportCompletionKeepsFailedDraftAndError)
{
    const auto target = InspectorTargetHandle::FromAsset(
        InspectorTargetKind::AssetImporter,
        "Assets/Models/FailedCompletion.fbx",
        "8ebfbb96-dc79-481c-8aa4-4740af3d9cbf");

    const auto state = TestHooks::CompleteAssetImporterReimportForTesting(
        target,
        11u,
        11u,
        {{"MODEL_SCALE", "1.0"}},
        std::map<std::string, std::string> {{"MODEL_SCALE", "2.0"}},
        false,
        "Synthetic importer failure.");

    EXPECT_TRUE(state.available);
    EXPECT_TRUE(state.dirty);
    EXPECT_FALSE(state.reimportQueued);
    EXPECT_EQ(state.revision, 12u);
    ASSERT_EQ(state.settings.size(), 1u);
    EXPECT_EQ(state.settings.at("MODEL_SCALE"), "2.0");
    EXPECT_EQ(state.lastError, "Synthetic importer failure.");
}

TEST(InspectorCoreTests, ImporterDraftUsesDirectByteStreamAndGlobalUndoWithoutPropertyBindings)
{
    ScopedInspectorUndoService undoScope;
    const auto target = InspectorTargetHandle::FromAsset(
        InspectorTargetKind::AssetImporter,
        "Assets/Models/DirectStream.fbx",
        "c15ee2d1-f6e9-43f8-a40e-1610aab74e86");
    TestHooks::SeedImporterDraftForTesting(target, {
        {"ENABLED", "true"},
        {"MIN_LOD", "0"},
        {"MODEL_SCALE", "1.0"},
        {"LABEL", "Original"}
    });
    struct ScopedImporterDraft final
    {
        InspectorTargetHandle target;
        ~ScopedImporterDraft() { TestHooks::ClearImporterDraftForTesting(target); }
    } cleanup {target};

    InspectorTargetSet targets;
    targets.targets.push_back(target);
    const auto presentation = AssetTargetAdapter().BuildPresentation(targets, {});
    ASSERT_EQ(presentation.roots.size(), 1u);
    EXPECT_TRUE(presentation.roots.front().children.empty());
    const auto hasMinLodProjection = [](const auto& self, const std::vector<PropertyNode>& nodes) -> bool
    {
        for (const auto& property : nodes)
        {
            if (property.fieldName == "MIN_LOD")
                return true;
            if (self(self, property.children))
                return true;
        }
        return false;
    };
    Serialization::SerializedObject object(
        targets,
        Serialization::ImporterDraftSerializedBackend::Create());
    const auto projection = InspectorProjectionBuilder::BuildPropertyProjection(object, targets, {});
    EXPECT_TRUE(hasMinLodProjection(hasMinLodProjection, projection));
    const auto scale = object.FindProperty("Importer.Settings.MODEL_SCALE");
    const auto minLod = object.FindProperty("Importer.Settings.MIN_LOD");
    ASSERT_TRUE(scale);
    ASSERT_TRUE(minLod);
    EXPECT_EQ(scale.GetNumericType(), Serialization::SerializedNumericType::Double);
    const auto initialScale = scale.GetValue();
    ASSERT_TRUE(initialScale.IsValid());
    EXPECT_EQ(initialScale.GetType(), NLS_TYPEOF(double));
    EXPECT_DOUBLE_EQ(initialScale.GetValue<double>(), 1.0);
    const auto* initialStream = object.GetPrimaryByteStream();
    ASSERT_NE(initialStream, nullptr);
    ASSERT_GE(initialStream->GetBytes().size(), 32u);
    double initialRawScale = 0.0;
    std::memcpy(&initialRawScale, initialStream->GetBytes().data() + 24u, sizeof(initialRawScale));
    EXPECT_DOUBLE_EQ(initialRawScale, 1.0);
    EXPECT_DOUBLE_EQ(scale.GetDoubleValue(), 1.0);
    EXPECT_EQ(minLod.GetLongValue(), 0);

    ASSERT_TRUE(scale.SetDoubleValue(2.5));
    ASSERT_TRUE(minLod.SetLongValue(-4));
    EXPECT_TRUE(object.HasModifiedProperties());
    // Setters edit the staged stream only; a fresh object still sees the live
    // in-memory draft until ApplyModifiedProperties commits one undo command.
    Serialization::SerializedObject beforeApply(
        targets,
        Serialization::ImporterDraftSerializedBackend::Create());
    EXPECT_DOUBLE_EQ(beforeApply.FindProperty("Importer.Settings.MODEL_SCALE").GetDoubleValue(), 1.0);
    ASSERT_TRUE(object.ApplyModifiedProperties());
    EXPECT_FALSE(object.HasModifiedProperties());

    Serialization::SerializedObject afterApply(
        targets,
        Serialization::ImporterDraftSerializedBackend::Create());
    EXPECT_DOUBLE_EQ(afterApply.FindProperty("Importer.Settings.MODEL_SCALE").GetDoubleValue(), 2.5);
    // Importer validation normalizes model LODs without touching persistent
    // metadata; the dedicated Apply/Reimport action remains the file writer.
    EXPECT_EQ(afterApply.FindProperty("Importer.Settings.MIN_LOD").GetLongValue(), 0);

    auto& undo = undoScope.Get();
    ASSERT_TRUE(undo.CanUndo());
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    Serialization::SerializedObject afterUndo(
        targets,
        Serialization::ImporterDraftSerializedBackend::Create());
    EXPECT_DOUBLE_EQ(afterUndo.FindProperty("Importer.Settings.MODEL_SCALE").GetDoubleValue(), 1.0);
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    Serialization::SerializedObject afterRedo(
        targets,
        Serialization::ImporterDraftSerializedBackend::Create());
    EXPECT_DOUBLE_EQ(afterRedo.FindProperty("Importer.Settings.MODEL_SCALE").GetDoubleValue(), 2.5);
}

TEST(InspectorCoreTests, MaterialUsesDirectByteStreamAndGlobalUndoWithoutPropertyBindings)
{
    ScopedInspectorUndoService undoScope;
    const auto target = InspectorTargetHandle::FromAsset(
        InspectorTargetKind::Material,
        "Assets/Materials/DirectStream.mat",
        "b0f7f07f-2ae8-4b5e-a9a4-4f517d3f3fa0");
    NLS::Render::Resources::Material material;
    material.path = target.resourcePath;
    material.SetRawParameter("Enabled", true);
    material.SetRawParameter("RenderQueue", int {2000});
    material.SetRawParameter("Roughness", 0.25f);
    material.SetRawParameter("Tint", NLS::Maths::Vector3 {1.0f, 0.5f, 0.25f});
    material.SetRawParameter("Albedo", std::string {"Texture2D"});
    material.SetTextureResourcePath("Albedo", "Assets/Textures/Original.png");

    struct ScopedMaterialResolver final
    {
        ScopedMaterialResolver(InspectorTargetHandle configuredTarget,
            NLS::Render::Resources::Material& configuredMaterial)
        {
            static_cast<void>(configuredTarget);
            Serialization::MaterialSerializedBackend::TargetResolverForTesting resolver(
                [&configuredMaterial](const Serialization::SerializedTargetHandle&)
                    -> NLS::Render::Resources::Material*
                {
                    return &configuredMaterial;
                });
            Serialization::MaterialSerializedBackend::SetTargetResolverForTesting(std::move(resolver));
        }

        ~ScopedMaterialResolver()
        {
            Serialization::MaterialSerializedBackend::SetTargetResolverForTesting({});
        }
    } resolver {target, material};

    InspectorTargetSet targets;
    targets.targets.push_back(target);
    Serialization::SerializedObject object(
        targets,
        Serialization::MaterialSerializedBackend::Create());
    ASSERT_TRUE(object.IsValid()) << object.GetLastError();
    const auto roughness = object.FindProperty("Material.Uniforms.Roughness");
    const auto tint = object.FindProperty("Material.Uniforms.Tint");
    const auto albedo = object.FindProperty("Material.Uniforms.Albedo");
    ASSERT_TRUE(roughness);
    ASSERT_TRUE(tint);
    ASSERT_TRUE(albedo);
    EXPECT_FLOAT_EQ(roughness.GetFloatValue(), 0.25f);
    EXPECT_TRUE(albedo.IsReadOnly());
    EXPECT_FALSE(albedo.SetStringValue("Assets/Textures/Changed.png"));

    ASSERT_TRUE(roughness.SetFloatValue(0.75f));
    ASSERT_TRUE(tint.SetValue(meta::Variant(NLS::Maths::Vector3 {0.1f, 0.2f, 0.3f})));
    EXPECT_TRUE(object.HasModifiedProperties());
    EXPECT_FLOAT_EQ(std::any_cast<float>(material.GetUniformsData().at("Roughness")), 0.25f);
    ASSERT_TRUE(object.ApplyModifiedProperties()) << object.GetLastError();
    EXPECT_FLOAT_EQ(std::any_cast<float>(material.GetUniformsData().at("Roughness")), 0.75f);
    const auto appliedTint = std::any_cast<NLS::Maths::Vector3>(material.GetUniformsData().at("Tint"));
    EXPECT_FLOAT_EQ(appliedTint.x, 0.1f);
    EXPECT_FLOAT_EQ(appliedTint.y, 0.2f);
    EXPECT_FLOAT_EQ(appliedTint.z, 0.3f);
    EXPECT_EQ(material.GetTextureResourcePath("Albedo"), "Assets/Textures/Original.png");

    auto& undo = undoScope.Get();
    ASSERT_TRUE(undo.CanUndo());
    const auto scope = MakeInspectorUndoScope(target);
    EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());
    // Save moves the material scope's clean baseline without discarding global
    // history. Redo returns to that baseline; Reload then invalidates only the
    // material's entries.
    undo.MarkSaved(scope, "material-save");
    EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_FLOAT_EQ(std::any_cast<float>(material.GetUniformsData().at("Roughness")), 0.25f);
    EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    EXPECT_FLOAT_EQ(std::any_cast<float>(material.GetUniformsData().at("Roughness")), 0.75f);
    EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());
    undo.InvalidateScope(scope, Undo::UndoInvalidationReason::Reloaded);
    undo.MarkSaved(scope, "material-reload");
    EXPECT_FALSE(undo.CanUndo());
    EXPECT_FALSE(undo.CanRedo());
}

TEST(InspectorCoreTests, AssetBrowserReimportAndReloadInvalidateMatchingAssetScopes)
{
    const auto exerciseInvalidation = [](const char* assetId,
        const char* resourcePath,
        const auto notify)
    {
        ScopedInspectorUndoService undoScope;
        const auto target = InspectorTargetHandle::FromAsset(
            InspectorTargetKind::AssetImporter,
            resourcePath,
            assetId);
        TestHooks::SeedImporterDraftForTesting(target, {{"MODEL_SCALE", "1.0"}});
        struct ScopedDraft final
        {
            InspectorTargetHandle target;
            ~ScopedDraft() { TestHooks::ClearImporterDraftForTesting(target); }
        } cleanup {target};

        InspectorTargetSet targets;
        targets.targets.push_back(target);
        targets.Normalize();
        Serialization::SerializedObject serialized(
            targets,
            Serialization::ImporterDraftSerializedBackend::Create());
        const auto scale = serialized.FindProperty("Importer.Settings.MODEL_SCALE");
        ASSERT_TRUE(scale);
        ASSERT_TRUE(scale.SetDoubleValue(2.0));
        ASSERT_TRUE(serialized.ApplyModifiedProperties()) << serialized.GetLastError();

        auto& undo = undoScope.Get();
        ASSERT_TRUE(undo.CanUndo());
        const auto scope = MakeInspectorUndoScope(target);
        EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());

        NLS::Editor::Assets::AssetBrowserItem item;
        item.sourceAssetPath = target.resourcePath;
        item.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetId));
        notify(item);

        EXPECT_FALSE(undo.CanUndo());
        EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());
    };

    exerciseInvalidation(
        "790e0e01-e020-4379-bb19-3d30a339cc12",
        "Assets/Models/BrowserScope.fbx",
        [](const NLS::Editor::Assets::AssetBrowserItem& item)
        {
            NLS::Editor::Panels::TestHooks::NotifyAssetBrowserReimportedForTesting(item);
        });
    exerciseInvalidation(
        "c2b0bf18-3e33-48f9-9af4-1b5ae2ca08f6",
        "Assets/Materials/BrowserScope.mat",
        [](const NLS::Editor::Assets::AssetBrowserItem& item)
        {
            NLS::Editor::Panels::TestHooks::NotifyAssetBrowserReloadedForTesting(item);
        });
}

TEST(InspectorCoreTests, AssetBrowserExternalRefreshInvalidatesMatchingAssetScopeWithoutSelection)
{
    ScopedInspectorUndoService undoScope;
    auto& undo = undoScope.Get();

    constexpr auto assetId = "c4c2da53-4798-4a75-ae71-bf2cd2e9b1e0";
    const auto target = InspectorTargetHandle::FromAsset(
        InspectorTargetKind::AssetImporter,
        "Assets/Textures/ExternalChange.png",
        assetId);
    TestHooks::SeedImporterDraftForTesting(target, {{"MODEL_SCALE", "1.0"}});
    struct ScopedDraft final
    {
        InspectorTargetHandle target;
        ~ScopedDraft() { TestHooks::ClearImporterDraftForTesting(target); }
    } cleanup {target};

    InspectorTargetSet targets;
    targets.targets.push_back(target);
    targets.Normalize();
    Serialization::SerializedObject serialized(
        targets,
        Serialization::ImporterDraftSerializedBackend::Create());
    const auto scale = serialized.FindProperty("Importer.Settings.MODEL_SCALE");
    ASSERT_TRUE(scale);
    ASSERT_TRUE(scale.SetDoubleValue(2.0));
    ASSERT_TRUE(serialized.ApplyModifiedProperties()) << serialized.GetLastError();
    ASSERT_TRUE(undo.CanUndo());

    NLS::Editor::Assets::AssetBrowserItem item;
    item.sourceAssetPath = target.resourcePath;
    item.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetId));
    // This mirrors a successfully completed external file-watcher preimport.
    // No EditorSelectionService state is installed by this test fixture.
    NLS::Editor::Panels::TestHooks::NotifyAssetBrowserExternalModificationForTesting(item);

    EXPECT_FALSE(undo.CanUndo());
    EXPECT_FALSE(undo.CanRedo());
    EXPECT_FALSE(undo.GetScopeState(MakeInspectorUndoScope(target)).IsDirty());
}

TEST(InspectorCoreTests, ProjectSettingsAdapterUsesStableGlobalUndoScopeAndSavePoints)
{
    NLS::meta::ReflectionDatabase::Instance();

    NLS::Editor::Settings::EditorSettingsRegistry registry;
    NLS::Editor::Settings::EditorSettings::RegisterSettingObjects(registry);
    ProjectSettingsTargetAdapter::BindRegistry(registry);
    struct ScopedRegistryBinding final
    {
        NLS::Editor::Settings::EditorSettingsRegistry& registry;
        ~ScopedRegistryBinding() { ProjectSettingsTargetAdapter::UnbindRegistry(registry); }
    } registryBinding {registry};

    auto& sceneTools = NLS::Editor::Settings::EditorSettings::GetSceneToolSettingsObject();
    const auto originalSceneTools = sceneTools;
    struct ScopedSceneToolRestore final
    {
        NLS::Editor::Settings::EditorSceneToolSettingsObject& settings;
        NLS::Editor::Settings::EditorSceneToolSettingsObject original;
        ~ScopedSceneToolRestore() { settings = original; }
    } sceneToolsRestore {sceneTools, originalSceneTools};

    InspectorTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromAsset(
        InspectorTargetKind::ProjectSettings,
        "editor.scene-tools",
        {},
        NLS_TYPEOF(NLS::Editor::Settings::EditorSceneToolSettingsObject).GetName()));

    const auto presentation = ProjectSettingsTargetAdapter().BuildPresentation(targets, {});
    ASSERT_EQ(presentation.roots.size(), 1u);
    EXPECT_TRUE(presentation.roots.front().children.empty());

    Serialization::SerializedObject serializedObject(
        targets,
        Serialization::ProjectSettingsSerializedBackend::Create());
    const auto projectionNodes = InspectorProjectionBuilder::BuildPropertyProjection(serializedObject, targets, {});
    const auto projection = std::find_if(
        projectionNodes.begin(),
        projectionNodes.end(),
        [](const PropertyNode& node) { return node.fieldName == "translationSnapUnit"; });
    ASSERT_NE(projection, projectionNodes.end());
    EXPECT_EQ(projection->path.ToString(), "translationSnapUnit");

    ScopedInspectorUndoService undoScope;
    auto& undo = undoScope.Get();
    const auto property = serializedObject.FindProperty("translationSnapUnit");
    ASSERT_TRUE(property);
    ASSERT_TRUE(property.SetFloatValue(2.5f));
    ASSERT_TRUE(serializedObject.ApplyModifiedProperties());
    EXPECT_FLOAT_EQ(sceneTools.translationSnapUnit, 2.5f);

    const auto scope = MakeInspectorUndoScope(projection->target);
    EXPECT_EQ(scope.kind, Undo::UndoScopeKind::ProjectSettings);
    EXPECT_EQ(scope.stableKey, "path:editor.scene-tools");
    EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());

    undo.MarkSaved(scope);
    EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_FLOAT_EQ(sceneTools.translationSnapUnit, originalSceneTools.translationSnapUnit);
    EXPECT_TRUE(undo.GetScopeState(scope).IsDirty());
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    EXPECT_FLOAT_EQ(sceneTools.translationSnapUnit, 2.5f);
    EXPECT_FALSE(undo.GetScopeState(scope).IsDirty());
}

TEST(InspectorCoreTests, MultiTargetEditRestoresEachOriginalValueWithOneUndoCommand)
{
    ScopedInspectorUndoService undoScope;
    auto& undo = undoScope.Get();

    Engine::GameObject first("First");
    Engine::GameObject second("Second");
    first.SetLayer(2);
    second.SetLayer(9);
    InspectorTargetSet targets;
    targets.targets = {
        InspectorTargetHandle::FromObject(first, InspectorTargetKind::GameObject),
        InspectorTargetHandle::FromObject(second, InspectorTargetKind::GameObject)};
    targets.Normalize();
    Serialization::SerializedObject serialized(
        targets,
        Serialization::AccessorSerializedBackend::Create());
    ASSERT_TRUE(serialized.IsEditingMultipleObjects());
    const auto layer = serialized.FindProperty("GameObject.layer");
    ASSERT_TRUE(layer);
    EXPECT_TRUE(layer.HasMultipleDifferentValues());
    ASSERT_TRUE(layer.SetLongValue(14));
    ASSERT_TRUE(serialized.ApplyModifiedProperties()) << serialized.GetLastError();
    EXPECT_EQ(first.GetLayer(), 14);
    EXPECT_EQ(second.GetLayer(), 14);
    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_EQ(first.GetLayer(), 2);
    EXPECT_EQ(second.GetLayer(), 9);
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    EXPECT_EQ(first.GetLayer(), 14);
    EXPECT_EQ(second.GetLayer(), 14);
}

TEST(InspectorCoreTests, StructuralSnapshotRestoresComponentOrderAndEnabledState)
{
    Engine::GameObject gameObject("Inspector snapshot");
    auto* light = gameObject.AddComponent<Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetEnabled(false);

    const auto snapshot = CaptureComponentStructure(gameObject);
    ASSERT_EQ(snapshot.components.size(), 2u);
    ASSERT_TRUE(gameObject.RemoveComponent(light));
    ASSERT_EQ(gameObject.GetComponents().size(), 1u);

    const Undo::UndoExecutionContext recoveryContext {
        0u,
        Undo::UndoDirection::Undo,
        0u,
        {}};
    const Undo::ScopedEditorUndoMutationContext mutationContext(recoveryContext);
    ASSERT_TRUE(RestoreComponentStructure(gameObject, snapshot));
    ASSERT_EQ(gameObject.GetComponents().size(), 2u);
    auto* restored = gameObject.GetComponent(NLS_TYPEOF(Engine::Components::LightComponent), false);
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(restored->IsSelfEnabled());
}

TEST(InspectorCoreTests, PrefabMenuRevertUsesInjectedServicesAndRestoresThroughUndo)
{
    ScopedInspectorUndoService undoScope;
    auto& undo = undoScope.Get();

    constexpr auto assetGuid = "4d721600-5b7c-4a56-9f46-6b3c8ffbe050";
    Engine::GameObject sourceRoot("Inspector prefab source", "Prop");
    Assets::PrefabEditorWorkflow workflow;
    auto artifact = workflow.CreatePrefabFromSelection({
        &sourceRoot,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetGuid)),
        "Assets/Prefabs/InspectorMenu.prefab"}).artifact;
    ASSERT_TRUE(artifact.has_value());

    Engine::SceneSystem::Scene scene;
    auto instantiated = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetGuid)),
        "prefab:InspectorMenu"}, scene);
    ASSERT_TRUE(instantiated.instance.has_value());

    Assets::PrefabInstanceRegistry registry;
    auto& instance = registry.Register(std::move(*instantiated.instance));
    ASSERT_NE(instance.instanceRoot->AddComponent<Engine::Components::LightComponent>(), nullptr);

    InspectorTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(
        *instance.instanceRoot,
        InspectorTargetKind::GameObject));
    targets.Normalize();
    TestHooks::ScopedPrefabInspectorServicesForTesting services(registry);

    const auto actions = GetPrefabInspectorActions(targets);
    const auto revert = std::find_if(actions.begin(), actions.end(), [](const PrefabInspectorAction& action)
    {
        return action.kind == PrefabInspectorActionKind::Revert;
    });
    ASSERT_NE(revert, actions.end());
    EXPECT_TRUE(revert->enabled);

    std::string error;
    ASSERT_TRUE(ExecutePrefabInspectorAction(targets, PrefabInspectorActionKind::Revert, error)) << error;
    EXPECT_EQ(instance.instanceRoot->GetComponent<Engine::Components::LightComponent>(), nullptr);
    ASSERT_TRUE(undo.CanUndo());

    ASSERT_TRUE(undo.RequestUndo().Succeeded());
    EXPECT_NE(instance.instanceRoot->GetComponent<Engine::Components::LightComponent>(), nullptr);
    const auto redo = undo.RequestRedo();
    ASSERT_TRUE(redo.Succeeded()) << redo.message;
    EXPECT_EQ(instance.instanceRoot->GetComponent<Engine::Components::LightComponent>(), nullptr);
}

TEST(InspectorCoreTests, PrefabMenuApplyExecutesThroughUndoAndRoundTripsSourceAndInstance)
{
    ScopedInspectorUndoService undoScope;
    auto& undo = undoScope.Get();
    TemporaryInspectorAssetProject project;

    constexpr auto assetGuid = "c72016a5-d07c-4a40-8c1e-2adff4f1e020";
    constexpr auto assetPath = "Assets/Prefabs/InspectorApply.prefab";
    constexpr auto subAssetKey = "prefab:InspectorApply";
    const auto prefabId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetGuid));
    Engine::GameObject sourceRoot("Inspector source", "Prop");
    Assets::PrefabEditorWorkflow workflow;
    const auto created = workflow.CreatePrefabFromSelection({
        &sourceRoot,
        {},
        prefabId,
        assetPath});
    ASSERT_TRUE(created.artifact.has_value());
    ASSERT_FALSE(created.prefabSourceText.empty());

    Assets::AssetDatabaseFacade sourceDatabase(Assets::MakeProjectEditorAssetRoots(project.root));
    ASSERT_TRUE(sourceDatabase.Refresh());
    ASSERT_TRUE(sourceDatabase.CreateTextAsset(created.prefabSourceText, assetPath, prefabId));
    ASSERT_TRUE(sourceDatabase.ImportAsset(assetPath));
    auto importedPrefab = sourceDatabase.LoadPrefabArtifactAtPath(assetPath, subAssetKey);
    ASSERT_TRUE(importedPrefab.has_value());

    Engine::SceneSystem::Scene scene;
    auto instantiated = workflow.InstantiatePrefab({
        &*importedPrefab,
        prefabId,
        subAssetKey}, scene);
    ASSERT_TRUE(instantiated.instance.has_value());

    Assets::PrefabInstanceRegistry registry;
    auto& instance = registry.Register(std::move(*instantiated.instance));
    instance.instanceRoot->SetName("Inspector applied name");

    InspectorTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(
        *instance.instanceRoot,
        InspectorTargetKind::GameObject));
    targets.Normalize();
    TestHooks::ScopedPrefabInspectorServicesForTesting services(registry, project.root / "Assets");

    const auto sourceFile = project.root / "Assets" / "Prefabs" / "InspectorApply.prefab";
    const auto sourceBefore = ReadTextFileForTesting(sourceFile);
    ASSERT_FALSE(sourceBefore.empty());
    const auto actionsBeforeApply = GetPrefabInspectorActions(targets);
    const auto apply = std::find_if(actionsBeforeApply.begin(), actionsBeforeApply.end(), [](const PrefabInspectorAction& action)
    {
        return action.kind == PrefabInspectorActionKind::Apply;
    });
    ASSERT_NE(apply, actionsBeforeApply.end());
    ASSERT_TRUE(apply->enabled);

    std::string error;
    ASSERT_TRUE(ExecutePrefabInspectorAction(targets, PrefabInspectorActionKind::Apply, error)) << error;
    const auto sourceAfter = ReadTextFileForTesting(sourceFile);
    EXPECT_NE(sourceAfter, sourceBefore);
    EXPECT_EQ(instance.instanceRoot->GetName(), "Inspector applied name");
    EXPECT_TRUE(GetPrefabInspectorActions(targets).empty());
    ASSERT_TRUE(undo.CanUndo());

    const auto undoResult = undo.RequestUndo();
    ASSERT_TRUE(undoResult.Succeeded()) << undoResult.message;
    EXPECT_EQ(ReadTextFileForTesting(sourceFile), sourceBefore);
    EXPECT_EQ(instance.instanceRoot->GetName(), "Inspector applied name");
    EXPECT_FALSE(GetPrefabInspectorActions(targets).empty());

    const auto redoResult = undo.RequestRedo();
    ASSERT_TRUE(redoResult.Succeeded()) << redoResult.message;
    EXPECT_EQ(ReadTextFileForTesting(sourceFile), sourceAfter);
    EXPECT_EQ(instance.instanceRoot->GetName(), "Inspector applied name");
    EXPECT_TRUE(GetPrefabInspectorActions(targets).empty());
}

TEST(InspectorCoreTests, PrefabRevertRestoresAppliedStateWhenGlobalUndoRejectsRecording)
{
    ScopedInspectorUndoService undoScope;
    auto& undo = undoScope.Get();

    constexpr auto assetGuid = "f6eaa9d5-07cb-4a40-858a-2adff4f1ebd1";
    Engine::GameObject sourceRoot("Prefab rollback source", "Prop");
    Assets::PrefabEditorWorkflow workflow;
    auto artifact = workflow.CreatePrefabFromSelection({
        &sourceRoot,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetGuid)),
        "Assets/Prefabs/PrefabUndoRecordRejected.prefab"}).artifact;
    ASSERT_TRUE(artifact.has_value());

    Engine::SceneSystem::Scene scene;
    auto instantiated = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetGuid)),
        "prefab:PrefabUndoRecordRejected"}, scene);
    ASSERT_TRUE(instantiated.instance.has_value());

    Assets::PrefabInstanceRegistry registry;
    auto& instance = registry.Register(std::move(*instantiated.instance));
    ASSERT_NE(instance.instanceRoot->AddComponent<Engine::Components::LightComponent>(), nullptr);

    InspectorTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(
        *instance.instanceRoot,
        InspectorTargetKind::GameObject));
    targets.Normalize();
    TestHooks::ScopedPrefabInspectorServicesForTesting services(registry);

    undo.Freeze(Undo::UndoFreezeReason::User);
    std::string error;
    EXPECT_FALSE(ExecutePrefabInspectorAction(targets, PrefabInspectorActionKind::Revert, error));
    EXPECT_NE(error.find("rejected"), std::string::npos);
    EXPECT_NE(instance.instanceRoot->GetComponent<Engine::Components::LightComponent>(), nullptr);
    EXPECT_FALSE(undo.CanUndo());

    const auto restoredActions = GetPrefabInspectorActions(targets);
    const auto revert = std::find_if(restoredActions.begin(), restoredActions.end(), [](const PrefabInspectorAction& action)
    {
        return action.kind == PrefabInspectorActionKind::Revert;
    });
    ASSERT_NE(revert, restoredActions.end());
    EXPECT_TRUE(revert->enabled);
    undo.Unfreeze(Undo::UndoFreezeReason::User);
}

TEST(InspectorCoreTests, PrefabUndoRevalidatesObjectGraphIdentityBeforeApply)
{
    ScopedInspectorUndoService undoScope;
    auto& undo = undoScope.Get();

    constexpr auto assetGuid = "aac04aa5-d552-4eaf-9814-a98d6d4f47cf";
    Engine::GameObject sourceRoot("Prefab identity source", "Prop");
    Assets::PrefabEditorWorkflow workflow;
    auto artifact = workflow.CreatePrefabFromSelection({
        &sourceRoot,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetGuid)),
        "Assets/Prefabs/PrefabUndoIdentity.prefab"}).artifact;
    ASSERT_TRUE(artifact.has_value());

    Engine::SceneSystem::Scene scene;
    auto instantiated = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetGuid)),
        "prefab:PrefabUndoIdentity"}, scene);
    ASSERT_TRUE(instantiated.instance.has_value());

    Assets::PrefabInstanceRegistry registry;
    auto& instance = registry.Register(std::move(*instantiated.instance));
    ASSERT_NE(instance.instanceRoot->AddComponent<Engine::Components::LightComponent>(), nullptr);
    const auto sourceRootId = instance.SourceGraph().root;
    ASSERT_TRUE(sourceRootId.IsValid());
    ASSERT_NE(instance.sourceToInstance.find(sourceRootId), instance.sourceToInstance.end());

    InspectorTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(
        *instance.instanceRoot,
        InspectorTargetKind::GameObject));
    targets.Normalize();
    TestHooks::ScopedPrefabInspectorServicesForTesting services(registry);

    std::string error;
    ASSERT_TRUE(ExecutePrefabInspectorAction(targets, PrefabInspectorActionKind::Revert, error)) << error;
    ASSERT_TRUE(undo.CanUndo());

    // The command persists the ObjectGraph root identity, not this live root's
    // InstanceID. Once the registry can no longer resolve that identity, Undo
    // must report Stale and stop instead of acting on a possibly reused object.
    instance.sourceToInstance.erase(sourceRootId);
    EXPECT_EQ(undo.RequestUndo().state, Undo::UndoRequestState::Stale);
    EXPECT_FALSE(undo.CanUndo());
}

TEST(InspectorCoreTests, SerializedValueRebindsPPtrRawAndNestedReferencesAfterObjectRebuild)
{
    NLS::meta::ReflectionDatabase::Instance();
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();

    auto primary = std::make_unique<Engine::GameObject>("Snapshot primary");
    auto secondary = std::make_unique<Engine::GameObject>("Snapshot secondary");
    auto* component = primary->AddComponent<Engine::Components::LightComponent>();
    ASSERT_NE(component, nullptr);

    StableReferenceFixture fixture;
    fixture.primary = NLS::Engine::Serialize::PPtr<Engine::GameObject>(primary.get());
    fixture.secondary = secondary.get();
    fixture.component = component;
    fixture.references = {
        NLS::Engine::Serialize::PPtr<Engine::GameObject>(primary.get()),
        NLS::Engine::Serialize::PPtr<Engine::GameObject>(secondary.get()),
        NLS::Engine::Serialize::PPtr<Engine::GameObject>()};

    const auto pptrFieldSnapshot = Serialization::SerializedValue::Capture(meta::Variant(fixture.primary));
    ASSERT_TRUE(pptrFieldSnapshot.has_value());
    const auto rawFieldSnapshot = Serialization::SerializedValue::Capture(meta::Variant(fixture.secondary));
    ASSERT_TRUE(rawFieldSnapshot.has_value());
    const auto arrayFieldSnapshot = Serialization::SerializedValue::Capture(meta::Variant(fixture.references));
    ASSERT_TRUE(arrayFieldSnapshot.has_value());
    const auto snapshot = Serialization::SerializedValue::Capture(meta::Variant(fixture));
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->IsValid());
    // The native reference table interns equal identifier/type pairs. A null
    // reference is slot zero in the byte stream and intentionally has no table
    // entry; the raw and PPtr forms of the secondary use different declared
    // reference types, so they retain separate relocatable entries.
    ASSERT_EQ(snapshot->references.size(), 4u);
    EXPECT_FALSE(snapshot->references[0].isNull);
    EXPECT_FALSE(snapshot->references[1].isNull);
    EXPECT_FALSE(snapshot->references[2].isNull);
    EXPECT_TRUE(std::none_of(
        snapshot->references.begin(),
        snapshot->references.end(),
        [](const Serialization::SerializedObjectReference& reference) { return reference.isNull; }));

    const auto primaryIdentifier = snapshot->references[0].identifier;
    const auto secondaryIdentifier = snapshot->references[1].identifier;
    const auto componentIdentifier = snapshot->references[2].identifier;
    ASSERT_TRUE(primaryIdentifier.IsValid());
    ASSERT_TRUE(secondaryIdentifier.IsValid());
    ASSERT_TRUE(componentIdentifier.IsValid());

    // Rebuild all objects so every original InstanceID is gone. The stable
    // identifiers are then bound to the replacement objects, exactly as a
    // scene reload/restore would do.
    primary.reset();
    secondary.reset();

    auto rebuiltPrimary = std::make_unique<Engine::GameObject>("Rebuilt primary");
    auto rebuiltSecondary = std::make_unique<Engine::GameObject>("Rebuilt secondary");
    auto* rebuiltComponent = rebuiltPrimary->AddComponent<Engine::Components::LightComponent>();
    ASSERT_NE(rebuiltComponent, nullptr);
    ASSERT_NE(
        persistentManager.BindObjectIdentifier(*rebuiltPrimary, primaryIdentifier),
        NLS::Engine::Serialize::InstanceID_None);
    ASSERT_NE(
        persistentManager.BindObjectIdentifier(*rebuiltSecondary, secondaryIdentifier),
        NLS::Engine::Serialize::InstanceID_None);
    ASSERT_NE(
        persistentManager.BindObjectIdentifier(*rebuiltComponent, componentIdentifier),
        NLS::Engine::Serialize::InstanceID_None);

    const auto resolved = snapshot->Resolve(NLS_TYPEOF(StableReferenceFixture));
    ASSERT_TRUE(resolved.has_value());
    ASSERT_TRUE(resolved->IsValid());
    const auto& restored = resolved->GetValue<StableReferenceFixture>();
    EXPECT_EQ(restored.primary.Get(), rebuiltPrimary.get());
    EXPECT_EQ(restored.secondary, rebuiltSecondary.get());
    EXPECT_EQ(restored.component, rebuiltComponent);
    ASSERT_EQ(restored.references.size(), 3u);
    EXPECT_EQ(restored.references[0].Get(), rebuiltPrimary.get());
    EXPECT_EQ(restored.references[1].Get(), rebuiltSecondary.get());
    EXPECT_TRUE(restored.references[2].IsNull());
}

TEST(InspectorCoreTests, SerializedValueRebindsAssetPointerByPersistentIdentifierAndPreservesNull)
{
    NLS::meta::ReflectionDatabase::Instance();
    auto& persistentManager = NLS::Engine::Serialize::PersistentManager::Instance();
    const auto assetIdentifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("7ef9d7c0-9f96-4f47-9f9c-507126ec3e84")),
        2100000,
        "Assets/Materials/SnapshotMaterial.mat");

    auto material = std::make_unique<NLS::Render::Resources::Material>();
    material->path = "Assets/Materials/SnapshotMaterial.mat";
    ASSERT_NE(
        persistentManager.BindObjectIdentifier(*material, assetIdentifier),
        NLS::Engine::Serialize::InstanceID_None);

    const auto snapshot = Serialization::SerializedValue::Capture(meta::Variant(material.get()));
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->IsValid());
    ASSERT_EQ(snapshot->references.size(), 1u);
    EXPECT_EQ(snapshot->references.front().identifier, assetIdentifier);
    EXPECT_EQ(snapshot->references.front().identifier.filePath, material->path);

    material.reset();
    auto rebuiltMaterial = std::make_unique<NLS::Render::Resources::Material>();
    rebuiltMaterial->path = "Assets/Materials/SnapshotMaterial.mat";
    ASSERT_NE(
        persistentManager.BindObjectIdentifier(*rebuiltMaterial, assetIdentifier),
        NLS::Engine::Serialize::InstanceID_None);

    const auto resolved = snapshot->Resolve(NLS_TYPEOF(NLS::Render::Resources::Material*));
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->GetValue<NLS::Render::Resources::Material*>(), rebuiltMaterial.get());

    const auto nullSnapshot = Serialization::SerializedValue::Capture(
        meta::Variant(static_cast<NLS::Render::Resources::Material*>(nullptr)));
    ASSERT_TRUE(nullSnapshot.has_value());
    ASSERT_TRUE(nullSnapshot->IsValid());
    EXPECT_TRUE(nullSnapshot->references.empty());
    const auto nullResolved = nullSnapshot->Resolve(
        NLS_TYPEOF(NLS::Render::Resources::Material*));
    ASSERT_TRUE(nullResolved.has_value());
    EXPECT_EQ(nullResolved->GetValue<NLS::Render::Resources::Material*>(), nullptr);
}

TEST(InspectorCoreTests, SceneReferenceClassificationAssignmentClearAndPingUseStableTargets)
{
    Engine::GameObject gameObject("Reference source");
    auto* pointer = &gameObject;
    meta::Variant value(pointer);
    const auto type = NLS_TYPEOF(Engine::GameObject*);

    EXPECT_TRUE(IsSupportedInspectorObjectReferenceType(type));
    EXPECT_TRUE(IsInspectorSceneObjectReferenceType(type));
    EXPECT_EQ(GetInspectorObjectReferenceDisplay(value, type), "Reference source (GameObject)");

    ASSERT_TRUE(ClearInspectorObjectReference(value, type));
    EXPECT_EQ(value.GetValue<Engine::GameObject*>(), nullptr);
    ASSERT_TRUE(AssignInspectorSceneReference(value, type, &gameObject));
    EXPECT_EQ(value.GetValue<Engine::GameObject*>(), &gameObject);

    const auto pingTarget = GetInspectorObjectReferencePingTarget(value, type);
    ASSERT_TRUE(pingTarget.has_value());
    EXPECT_EQ(pingTarget->kind, InspectorTargetKind::GameObject);
    EXPECT_EQ(pingTarget->instanceId, gameObject.GetInstanceID());
}

TEST(InspectorCoreTests, UIDocumentReferencesUseTheSharedAssetReferenceDrawerContract)
{
    const auto type = NLS_TYPEOF(Engine::UI::UIDocumentReference);
    meta::Variant value(Engine::UI::UIDocumentReference {});
    const auto documentId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("26fe2d03-6a80-4271-a67f-9cfa40173352"));
    const auto documentPayload = Assets::MakeEditorAssetDragPayload(
        "Assets/UI/Hud.ui",
        documentId,
        {},
        NLS::Core::Assets::ArtifactType::UIDocument,
        false,
        false);
    const auto texturePayload = Assets::MakeEditorAssetDragPayload(
        "Assets/Textures/Hud.png",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("6a80458b-32c7-4916-80aa-4b5fe733d0d3")),
        {},
        NLS::Core::Assets::ArtifactType::Texture,
        false,
        false);

    EXPECT_TRUE(IsSupportedInspectorObjectReferenceType(type));
    EXPECT_TRUE(IsInspectorAssetObjectReferenceType(type));
    EXPECT_TRUE(IsCompatibleInspectorReferenceArtifact(type, NLS::Core::Assets::ArtifactType::UIDocument));
    EXPECT_FALSE(IsCompatibleInspectorReferenceArtifact(type, NLS::Core::Assets::ArtifactType::Texture));
    EXPECT_EQ(GetInspectorObjectReferenceDisplay(value, type), "None (UI Document)");
    EXPECT_FALSE(AssignInspectorAssetReference(value, type, texturePayload));
    ASSERT_TRUE(AssignInspectorAssetReference(value, type, documentPayload));

    const auto& reference = value.GetValue<Engine::UI::UIDocumentReference>();
    EXPECT_EQ(reference.GetAssetId(), documentId);
    EXPECT_EQ(reference.GetResourcePath(), "Assets/UI/Hud.ui");
    EXPECT_EQ(GetInspectorObjectReferenceDisplay(value, type), "Hud.ui (UI Document)");
    const auto pingTarget = GetInspectorObjectReferencePingTarget(value, type);
    ASSERT_TRUE(pingTarget.has_value());
    EXPECT_EQ(pingTarget->kind, InspectorTargetKind::AssetImporter);
    EXPECT_EQ(pingTarget->assetId, documentId.ToString());
    EXPECT_EQ(pingTarget->resourcePath, "Assets/UI/Hud.ui");

    ASSERT_TRUE(ClearInspectorObjectReference(value, type));
    EXPECT_TRUE(value.GetValue<Engine::UI::UIDocumentReference>().IsNull());
}

TEST(InspectorCoreTests, CanvasDocumentProjectsAsASharedAssetReferenceField)
{
    Engine::GameObject object("Canvas Holder");
    auto* canvas = object.AddComponent<Engine::Components::UICanvasComponent>();
    ASSERT_NE(canvas, nullptr);

    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(*canvas, InspectorTargetKind::NativeObject));
    targets.Normalize();
    ASSERT_TRUE(Serialization::AccessorSerializedBackend::Supports(targets));

    Serialization::SerializedObject serialized(targets, Serialization::AccessorSerializedBackend::Create());
    ASSERT_TRUE(serialized.IsValid());

    const auto properties = InspectorProjectionBuilder::BuildPropertyProjection(serialized, targets, {});
    const auto findProperty = [](const auto& self, const std::vector<PropertyNode>& nodes,
                                 const std::string_view path) -> const PropertyNode*
    {
        for (const auto& node : nodes)
        {
            if (node.path.ToString() == path)
                return &node;
            if (const auto* child = self(self, node.children, path))
                return child;
        }
        return nullptr;
    };
    const auto* document = findProperty(findProperty, properties, "document");
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(document->valueType.IsValid());
    EXPECT_EQ(document->valueType, NLS_TYPEOF(Engine::UI::UIDocumentReference));
    // The UI document reference stores an AssetId and a resource path, but it is
    // still an asset reference. It must reach the shared object reference drawer
    // so the Inspector offers picker, drop target, Clear and Ping instead of a
    // struct whose child edits the accessor backend cannot commit.
    EXPECT_EQ(document->kind, InspectorPropertyKind::ObjectReference);

    const auto documentId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("b1f6f5b3-4a7c-4f18-9d2c-2f4b5c6a7d80"));
    const auto property = serialized.FindProperty("document");
    ASSERT_TRUE(property);
    ASSERT_TRUE(property.IsEditable());
    ASSERT_TRUE(property.SetValue(meta::Variant(Engine::UI::UIDocumentReference(documentId, "Assets/UI/Hud.ui"))));
    ASSERT_TRUE(serialized.ApplyModifiedPropertiesWithoutUndo());
    EXPECT_EQ(canvas->GetDocumentPath(), "Assets/UI/Hud.ui");
    EXPECT_EQ(canvas->GetDocumentReference().GetAssetId(), documentId);
    EXPECT_EQ(
        GetInspectorObjectReferenceDisplay(property.GetValue(), document->valueType),
        "Hud.ui (UI Document)");
}

TEST(InspectorCoreTests, AssigningUiDocumentReferenceLoadsTheDocumentFromTheResolver)
{
    const auto projectRoot = std::filesystem::temp_directory_path() /
        ("nullus_ui_canvas_assign_" + NLS::Guid::New().ToString());
    const auto documentPath = projectRoot / "Assets" / "UI" / "Hud.ui";
    std::filesystem::create_directories(documentPath.parent_path());
    {
        std::ofstream output(documentPath, std::ios::binary | std::ios::trunc);
        output << R"json({
            "version": 1,
            "documentId": "44444444-4444-4444-8444-444444444444",
            "root": { "type": "Panel", "size": [320, 120], "children": [
                { "type": "Text", "name": "Score", "text": "Score: 0" }
            ] }
        })json";
        ASSERT_TRUE(output.good());
    }

    Engine::UI::SetUIDocumentSourcePathResolver(
        [assetsPath = (projectRoot / "Assets").generic_string()](
            const Engine::UI::UIDocumentReference& reference)
        {
            return Engine::UI::ResolveProjectUIDocumentPath(
                assetsPath,
                reference.GetResourcePath());
        });

    Engine::GameObject object("Canvas Holder");
    auto* canvas = object.AddComponent<Engine::Components::UICanvasComponent>();
    ASSERT_NE(canvas, nullptr);

    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(InspectorTargetHandle::FromObject(*canvas, InspectorTargetKind::NativeObject));
    targets.Normalize();
    ASSERT_TRUE(Serialization::AccessorSerializedBackend::Supports(targets));

    Serialization::SerializedObject serialized(targets, Serialization::AccessorSerializedBackend::Create());
    ASSERT_TRUE(serialized.IsValid());

    // This is the Inspector drop path: the asset browser payload is converted
    // into a reference variant and committed through the serialized stream.
    const auto payload = Assets::MakeEditorAssetDragPayload(
        "Assets/UI/Hud.ui",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("b3030303-0303-4303-8303-030303030303")),
        "ui-document:Hud",
        NLS::Core::Assets::ArtifactType::UIDocument,
        false,
        false,
        false);
    ASSERT_TRUE(IsCompatibleInspectorReferenceArtifact(
        NLS_TYPEOF(Engine::UI::UIDocumentReference),
        Assets::GetEditorAssetDragPayloadArtifactType(payload)));

    const auto property = serialized.FindProperty("document");
    ASSERT_TRUE(property);
    ASSERT_TRUE(property.IsEditable());
    auto value = property.GetValue();
    ASSERT_TRUE(AssignInspectorAssetReference(
        value,
        NLS_TYPEOF(Engine::UI::UIDocumentReference),
        payload));
    ASSERT_TRUE(property.SetValue(value));
    ASSERT_TRUE(serialized.ApplyModifiedPropertiesWithoutUndo());

    // Assigning must resolve into a live document, not just store a path.
    EXPECT_EQ(canvas->GetDocumentPath(), "Assets/UI/Hud.ui");
    ASSERT_NE(canvas->GetDocument(), nullptr);
    EXPECT_NE(canvas->GetElementHandle("Score"), 0u);

    Engine::UI::SetUIDocumentSourcePathResolver({});
    std::filesystem::remove_all(projectRoot);
}

} // namespace NLS::Editor::Inspector::Tests
