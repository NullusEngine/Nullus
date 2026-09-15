#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <Components/CameraComponent.h>
#include <Components/UICanvasComponent.h>
#include <Reflection/Macros.h>
#include <Reflection/Type.h>
#include <Scripting/ScriptComponent.h>

#include "Inspector/InspectorRegistry.h"
#include "Scripting/EditorScriptAbi.h"
#include "Scripting/EditorScriptHost.h"

namespace
{
constexpr const char* kScriptIdentifierPrefix = "script:";

// A scripted registration identifier is "script:<generation>:<editor type>".
// The generation makes a reloaded assembly produce new identifiers so the
// Inspector rebuilds its editors instead of pinning the previous assembly.
void ExpectScriptedIdentifier(
    const NLS::Editor::Inspector::ComponentEditorRegistration& registration,
    std::string_view editorTypeName)
{
    const std::string expectedSuffix = ":" + std::string(editorTypeName);
    EXPECT_EQ(registration.identifier.rfind(kScriptIdentifierPrefix, 0), 0u);
    EXPECT_EQ(registration.identifier.size() >= expectedSuffix.size()
            && registration.identifier.compare(
                registration.identifier.size() - expectedSuffix.size(),
                expectedSuffix.size(),
                expectedSuffix) == 0, true)
        << registration.identifier;
}

// Minimal stand-in for the native adapter that owns a scripted editor or
// window instance.
class StubScriptTarget final : public NLS::Editor::Scripting::IEditorScriptTarget
{
public:
    [[nodiscard]] bool IsInspectorTarget() const override { return true; }
};

class EditorScriptTests : public ::testing::Test
{
protected:
    void TearDown() override
    {
        auto& host = NLS::Editor::Scripting::EditorScriptHost::Get();
        host.ResetManagedApi();
        NLS::Editor::Inspector::InspectorRegistry::Get()
            .UnregisterComponentEditorsByIdentifierPrefix(kScriptIdentifierPrefix);
    }
};
} // namespace

TEST_F(EditorScriptTests, HostAbiTableExposesEveryManagedCallback)
{
    const auto& table = NLS::Editor::Scripting::GetEditorScriptHostApiTable();
    EXPECT_EQ(table.header.abiVersion, 1u);
    EXPECT_EQ(table.header.size, sizeof(NLS::Editor::Scripting::EditorScriptHostApiTable));
    EXPECT_NE(table.registerComponentEditor, nullptr);
    EXPECT_NE(table.registerMenuItem, nullptr);
    EXPECT_NE(table.clearRegistrations, nullptr);
    EXPECT_NE(table.isHandleValid, nullptr);
    EXPECT_NE(table.getTargetCount, nullptr);
    EXPECT_NE(table.getTargetDisplayName, nullptr);
    EXPECT_NE(table.getTargetInstanceId, nullptr);
    EXPECT_NE(table.getSearchText, nullptr);
    EXPECT_NE(table.isMultiObject, nullptr);
    EXPECT_NE(table.reportError, nullptr);
    EXPECT_NE(table.propertyField, nullptr);
    EXPECT_NE(table.drawDefaultInspector, nullptr);
    EXPECT_NE(table.label, nullptr);
    EXPECT_NE(table.space, nullptr);
    EXPECT_NE(table.toggle, nullptr);
    EXPECT_NE(table.intField, nullptr);
    EXPECT_NE(table.floatField, nullptr);
    EXPECT_NE(table.slider, nullptr);
    EXPECT_NE(table.textField, nullptr);
    EXPECT_NE(table.button, nullptr);
    EXPECT_NE(table.helpBox, nullptr);
    EXPECT_NE(table.beginHorizontal, nullptr);
    EXPECT_NE(table.endHorizontal, nullptr);
    EXPECT_NE(table.beginVertical, nullptr);
    EXPECT_NE(table.endVertical, nullptr);
    EXPECT_NE(table.openWindow, nullptr);
    EXPECT_NE(table.closeWindow, nullptr);
}

TEST_F(EditorScriptTests, ScriptedEditorOverridesTheBuiltInEditorAtHigherPriority)
{
    const auto cameraType = NLS_TYPEOF(NLS::Engine::Components::CameraComponent);
    ASSERT_TRUE(cameraType.IsValid());

    auto& host = NLS::Editor::Scripting::EditorScriptHost::Get();
    std::string error;
    ASSERT_TRUE(host.RegisterComponentEditor(
        {"ScriptedEditorTests.CameraEditor", cameraType.GetName(), false, true, 100},
        error)) << error;

    host.ApplyComponentEditorRegistrations();

    auto& registry = NLS::Editor::Inspector::InspectorRegistry::Get();
    const auto resolved = registry.ResolveComponentEditor(cameraType);
    ASSERT_TRUE(resolved.has_value());
    ExpectScriptedIdentifier(*resolved, "ScriptedEditorTests.CameraEditor");
    EXPECT_TRUE(resolved->supportsMultiObject);
    EXPECT_EQ(resolved->priority, 100);

    // The factory must build a scripted editor, not a native one, and it must
    // do so before any managed table is installed.
    const auto editor = resolved->create();
    ASSERT_NE(editor, nullptr);
}

TEST_F(EditorScriptTests, UnresolvableTargetNameRegistersAsAScriptComponentTarget)
{
    // A project Behaviour has no distinct reflection type, so a target name that
    // is not a reflected engine component is registered as a script component
    // name instead of being rejected.
    auto& host = NLS::Editor::Scripting::EditorScriptHost::Get();
    std::string error;
    ASSERT_TRUE(host.RegisterComponentEditor(
        {"ScriptedEditorTests.ScriptEditor", "Nullus.GameScripts.DoesNotExist", false, false, 100},
        error)) << error;
    EXPECT_TRUE(error.empty());

    host.ApplyComponentEditorRegistrations();

    auto& registry = NLS::Editor::Inspector::InspectorRegistry::Get();
    const auto scriptResolved = registry.ResolveScriptComponentEditor("Nullus.GameScripts.DoesNotExist");
    ASSERT_TRUE(scriptResolved.has_value());
    ExpectScriptedIdentifier(*scriptResolved, "ScriptedEditorTests.ScriptEditor");
    EXPECT_EQ(scriptResolved->scriptTypeName, "Nullus.GameScripts.DoesNotExist");

    // It must not leak onto the shared ScriptComponent reflection type.
    const auto scriptComponentType = NLS_TYPEOF(NLS::Scripting::ScriptComponent);
    ASSERT_TRUE(scriptComponentType.IsValid());
    const auto typeResolved = registry.ResolveComponentEditor(scriptComponentType);
    if (typeResolved.has_value())
        EXPECT_EQ(typeResolved->identifier.rfind(kScriptIdentifierPrefix, 0), std::string::npos);
}

TEST_F(EditorScriptTests, EmptyDescriptorFieldsAreRejected)
{
    auto& host = NLS::Editor::Scripting::EditorScriptHost::Get();
    std::string error;
    EXPECT_FALSE(host.RegisterComponentEditor({"", "NLS::Engine::Components::CameraComponent", false, false, 100}, error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(host.RegisterComponentEditor({"ScriptedEditorTests.Editor", "", false, false, 100}, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(EditorScriptTests, ScriptedEditorRegistersForAScriptComponentTypeName)
{
    // Every C#/Lua script component shares the ScriptComponent reflection type,
    // so a scripted editor for a Behaviour is keyed by the script's own type
    // name and must not answer the shared component type.
    const auto scriptComponentType = NLS_TYPEOF(NLS::Scripting::ScriptComponent);
    ASSERT_TRUE(scriptComponentType.IsValid());

    auto& host = NLS::Editor::Scripting::EditorScriptHost::Get();
    std::string error;
    ASSERT_TRUE(host.RegisterComponentEditor(
        {"ScriptedEditorTests.RotatingCubeEditor", "Nullus.GameScripts.RotatingCube", false, false, 100},
        error)) << error;

    host.ApplyComponentEditorRegistrations();

    auto& registry = NLS::Editor::Inspector::InspectorRegistry::Get();
    const auto resolved = registry.ResolveScriptComponentEditor("Nullus.GameScripts.RotatingCube");
    ASSERT_TRUE(resolved.has_value());
    ExpectScriptedIdentifier(*resolved, "ScriptedEditorTests.RotatingCubeEditor");
    EXPECT_EQ(resolved->scriptTypeName, "Nullus.GameScripts.RotatingCube");

    const auto typeResolved = registry.ResolveComponentEditor(scriptComponentType);
    if (typeResolved.has_value())
        EXPECT_EQ(typeResolved->identifier.rfind(kScriptIdentifierPrefix, 0), std::string::npos)
            << "a script-targeted editor must not be reachable through the shared ScriptComponent type";

    EXPECT_FALSE(registry.ResolveScriptComponentEditor("Nullus.GameScripts.NotRegistered").has_value());
    EXPECT_FALSE(registry.ResolveScriptComponentEditor("").has_value());
}

TEST_F(EditorScriptTests, DuplicateDescriptorsAndMenuPathsAreRejected)
{
    auto& host = NLS::Editor::Scripting::EditorScriptHost::Get();
    std::string error;
    ASSERT_TRUE(host.RegisterComponentEditor(
        {"ScriptedEditorTests.DuplicateEditor", "NLS::Engine::Components::CameraComponent", false, false, 100},
        error));
    EXPECT_FALSE(host.RegisterComponentEditor(
        {"ScriptedEditorTests.DuplicateEditor", "NLS::Engine::Components::CameraComponent", false, false, 100},
        error));
    EXPECT_FALSE(error.empty());

    error.clear();
    ASSERT_TRUE(host.RegisterMenuItem({"Window/Scripted Audit", "ScriptedEditorTests.Window"}, error));
    EXPECT_FALSE(host.RegisterMenuItem({"Window/Scripted Audit", "ScriptedEditorTests.Other"}, error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(host.RegisterMenuItem({"", "ScriptedEditorTests.Window"}, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(EditorScriptTests, HandlesDetectStaleAndUnknownTargets)
{
    auto& host = NLS::Editor::Scripting::EditorScriptHost::Get();
    StubScriptTarget target;

    const auto handle = host.RegisterTarget(target);
    EXPECT_NE(handle, 0u);
    EXPECT_EQ(host.ResolveTarget(handle), &target);
    EXPECT_TRUE(host.IsHandleValid(handle));

    host.UnregisterTarget(handle);
    EXPECT_EQ(host.ResolveTarget(handle), nullptr);
    EXPECT_FALSE(host.IsHandleValid(handle));

    // A re-used slot must never resolve the retired handle, and unknown handles
    // must be rejected instead of dereferenced.
    const auto reused = host.RegisterTarget(target);
    EXPECT_NE(reused, handle);
    EXPECT_EQ(host.ResolveTarget(handle), nullptr);
    EXPECT_EQ(host.ResolveTarget(reused), &target);
    EXPECT_EQ(host.ResolveTarget(0u), nullptr);
    EXPECT_EQ(host.ResolveTarget(0xFFFFFFFFFFFFFFFFull), nullptr);

    host.UnregisterTarget(reused);
}

TEST_F(EditorScriptTests, HostTableRejectsWorkWhenNoTargetIsRegistered)
{
    const auto& table = NLS::Editor::Scripting::GetEditorScriptHostApiTable();

    EXPECT_EQ(table.isHandleValid(4242u), 0u);
    EXPECT_EQ(table.getTargetCount(4242u), 0u);
    EXPECT_EQ(table.isMultiObject(4242u), 0u);
    EXPECT_EQ(table.propertyField(4242u, "document", nullptr, nullptr, 1u), 0u);
    EXPECT_EQ(table.drawDefaultInspector(4242u), 0u);
    EXPECT_EQ(table.button(4242u, "Apply", nullptr, 1u), 0u);

    char buffer[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', '\0'};
    EXPECT_GT(table.getSearchText(4242u, buffer, sizeof(buffer)), 0u);
    EXPECT_EQ(buffer[0], '\0');
}

TEST_F(EditorScriptTests, RegistrationReplacementKeepsTheInspectorRegistryConsistent)
{
    const auto canvasType = NLS_TYPEOF(NLS::Engine::Components::UICanvasComponent);
    ASSERT_TRUE(canvasType.IsValid());

    auto& host = NLS::Editor::Scripting::EditorScriptHost::Get();
    std::string error;
    ASSERT_TRUE(host.RegisterComponentEditor(
        {"ScriptedEditorTests.CanvasEditorA", canvasType.GetName(), false, false, 100},
        error));
    host.ApplyComponentEditorRegistrations();

    auto& registry = NLS::Editor::Inspector::InspectorRegistry::Get();
    ASSERT_TRUE(registry.ResolveComponentEditor(canvasType).has_value());
    ExpectScriptedIdentifier(*registry.ResolveComponentEditor(canvasType), "ScriptedEditorTests.CanvasEditorA");

    // A domain reload re-registers from scratch. The registry must accept the
    // replacement instead of failing on a duplicate scripted identifier.
    host.ClearRegistrations();
    ASSERT_TRUE(host.RegisterComponentEditor(
        {"ScriptedEditorTests.CanvasEditorB", canvasType.GetName(), false, false, 100},
        error));
    host.ApplyComponentEditorRegistrations();

    const auto resolved = registry.ResolveComponentEditor(canvasType);
    ASSERT_TRUE(resolved.has_value());
    ExpectScriptedIdentifier(*resolved, "ScriptedEditorTests.CanvasEditorB");
}
