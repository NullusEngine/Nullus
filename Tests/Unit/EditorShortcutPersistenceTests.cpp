#include <filesystem>
#include <fstream>
#include <Json/json.hpp>

#include <gtest/gtest.h>

#include "Shortcuts/EditorShortcutProfile.h"
#include "EditorInputTestSupport.h"
using NLS::Tests::TestEditorShortcutService;
using NLS::Tests::TestShortcutCommand;

using namespace NLS::Editor::Shortcuts;
using namespace NLS::Windowing::Inputs;

namespace
{
std::filesystem::path TempProfilePath(const std::string& p_name)
{
    const auto dir = std::filesystem::temp_directory_path() / "NullusShortcutTests" / NLS::Guid::New().ToString();
    std::filesystem::create_directories(dir);
    return dir / p_name;
}

void WriteLegacyProfile(const std::filesystem::path& path,
    std::initializer_list<std::pair<const char*, ShortcutBinding>> bindings,
    std::initializer_list<const char*> unassigned = {})
{
    nlohmann::json root = {{"version", 1}, {"profileName", "Default"}, {"bindings", nlohmann::json::object()}, {"unassigned", unassigned}};
    for (const auto& [id, binding] : bindings)
        root["bindings"][id] = {{"assigned", binding.assigned}, {"key", std::to_string(static_cast<int>(binding.primaryKey))}, {"modifiers", binding.modifiers}};
    std::ofstream output(path); output << root.dump();
    ASSERT_TRUE(output.good());
}

TestShortcutCommand Command(std::string id, ShortcutBinding binding)
{
    TestShortcutCommand command;
    command.id = std::move(id);
    command.displayName = command.id;
    command.category = "Test";
    command.context = ShortcutContexts::Global;
    command.defaultBinding = binding;
    command.execute = [] {};
    return command;
}
}

TEST(EditorShortcutPersistenceTests, SavesAndLoadsBindingOverrides)
{
    const auto path = TempProfilePath("overrides.inputprofile");
    TestEditorShortcutService service;
    ASSERT_TRUE(service.RegisterCommand(Command("file.save", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl))));
    ASSERT_TRUE(service.SetBinding("file.save", ShortcutBinding::FromKey(EKey::KEY_D, EShortcutModifier::Ctrl)));
    ASSERT_TRUE(service.SaveProfile(path));
    TestEditorShortcutService loaded;
    ASSERT_TRUE(loaded.RegisterCommand(Command("file.save", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl))));
    ASSERT_TRUE(loaded.LoadProfile(path));
    EXPECT_EQ(loaded.GetBinding("file.save"), ShortcutBinding::FromKey(EKey::KEY_D, EShortcutModifier::Ctrl));
}

TEST(EditorShortcutPersistenceTests, PreservesLegacyPhysicalAndLogicalPrimaryModifiers)
{
    const auto path = TempProfilePath("primary-and-legacy-modifiers.json");
    std::filesystem::remove(path);

    WriteLegacyProfile(path, {
        {"legacy.ctrl", ShortcutBinding::FromKey(EKey::KEY_C, EShortcutModifier::Ctrl)},
        {"legacy.super", ShortcutBinding::FromKey(EKey::KEY_C, EShortcutModifier::Super)},
        {"logical.primary", ShortcutBinding::FromKey(EKey::KEY_C, EShortcutModifier::Primary)}});

    EditorShortcutProfile loaded;
    ASSERT_TRUE(loaded.Load(path));

    EXPECT_EQ(
        loaded.GetBindingOverride("legacy.ctrl"),
        ShortcutBinding::FromKey(EKey::KEY_C, EShortcutModifier::Ctrl));
    EXPECT_EQ(
        loaded.GetBindingOverride("legacy.super"),
        ShortcutBinding::FromKey(EKey::KEY_C, EShortcutModifier::Super));
    EXPECT_EQ(
        loaded.GetBindingOverride("logical.primary"),
        ShortcutBinding::FromKey(EKey::KEY_C, EShortcutModifier::Primary));
}

TEST(EditorShortcutPersistenceTests, LoadsVersionOnePhysicalModifierMasksWithoutMigration)
{
    const auto path = TempProfilePath("legacy-physical-modifiers.json");
    std::filesystem::remove(path);

    {
        std::ofstream output(path);
        output << R"({
  "version": 1,
  "profileName": "Default",
  "bindings": {
    "legacy.ctrl": { "assigned": true, "key": "67", "modifiers": 1 },
    "legacy.super": { "assigned": true, "key": "67", "modifiers": 8 }
  },
  "unassigned": []
})";
    }

    EditorShortcutProfile loaded;
    ASSERT_TRUE(loaded.Load(path));

    EXPECT_EQ(
        loaded.GetBindingOverride("legacy.ctrl"),
        ShortcutBinding::FromKey(EKey::KEY_C, EShortcutModifier::Ctrl));
    EXPECT_EQ(
        loaded.GetBindingOverride("legacy.super"),
        ShortcutBinding::FromKey(EKey::KEY_C, EShortcutModifier::Super));
}

TEST(EditorShortcutPersistenceTests, PersistsUnassignedCommands)
{
    const auto path = TempProfilePath("unassigned.json");
    std::filesystem::remove(path);

    WriteLegacyProfile(path, {}, {"file.save"});

    EditorShortcutProfile loaded;
    ASSERT_TRUE(loaded.Load(path));

    EXPECT_TRUE(loaded.IsCommandUnassigned("file.save"));
}

TEST(EditorShortcutPersistenceTests, MissingOrMalformedProfilesFallbackToEmptyProfile)
{
    const auto missingPath = TempProfilePath("missing.json");
    std::filesystem::remove(missingPath);

    EditorShortcutProfile missing;
    EXPECT_TRUE(missing.Load(missingPath));
    EXPECT_FALSE(missing.GetBindingOverride("file.save").has_value());

    const auto malformedPath = TempProfilePath("malformed.json");
    {
        std::ofstream out(malformedPath);
        out << "{ bad json";
    }

    EditorShortcutProfile malformed;
    EXPECT_FALSE(malformed.Load(malformedPath));
    EXPECT_FALSE(malformed.GetBindingOverride("file.save").has_value());
}

TEST(EditorShortcutPersistenceTests, ServiceAppliesLoadedOverridesAndUnassignedCommands)
{
    const auto path = TempProfilePath("service.json");
    std::filesystem::remove(path);

    WriteLegacyProfile(path, {{"file.save", ShortcutBinding::FromKey(EKey::KEY_D, EShortcutModifier::Ctrl)}}, {"file.new"});

    TestEditorShortcutService service;
    service.RegisterCommand(Command("file.save", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl)));
    service.RegisterCommand(Command("file.new", ShortcutBinding::FromKey(EKey::KEY_N, EShortcutModifier::Ctrl)));

    EXPECT_TRUE(service.LoadProfile(path.parent_path() / "editor.inputprofile", path));

    EXPECT_EQ(service.GetBinding("file.save"), ShortcutBinding::FromKey(EKey::KEY_D, EShortcutModifier::Ctrl));
    EXPECT_FALSE(service.GetBinding("file.new").assigned);
}

TEST(EditorShortcutPersistenceTests, ServiceAssignClearAndResetWorkflow)
{
    TestEditorShortcutService service;
    service.RegisterCommand(Command("file.save", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl)));

    ASSERT_TRUE(service.SetBinding("file.save", ShortcutBinding::FromKey(EKey::KEY_D, EShortcutModifier::Ctrl)));
    EXPECT_EQ(service.GetBindingDisplayText("file.save"), "Ctrl + D");

    ASSERT_TRUE(service.ClearBinding("file.save"));
    EXPECT_EQ(service.GetBindingDisplayText("file.save"), "Unassigned");

    ASSERT_TRUE(service.ResetBinding("file.save"));
    EXPECT_EQ(service.GetBindingDisplayText("file.save"), "Ctrl + S");
}

TEST(EditorShortcutPersistenceTests, ServiceDoesNotClearRequiredBindings)
{
    TestEditorShortcutService service;
    auto command = Command("editor.required", ShortcutBinding::FromKey(EKey::KEY_F5));
    command.requiredBinding = true;
    service.RegisterCommand(command);

    ASSERT_NE(service.GetRegistry().FindCommand("editor.required"), nullptr);
    EXPECT_TRUE(service.GetRegistry().FindCommand("editor.required")->requiredBinding);

    EXPECT_FALSE(service.ClearBinding("editor.required"));
    EXPECT_EQ(service.GetBindingDisplayText("editor.required"), "F5");
}

TEST(EditorShortcutPersistenceTests, ExistingInputProfileWinsAndLegacyFileIsNeverWritten)
{
    const auto path = TempProfilePath("editor.inputprofile");
    const auto legacy = path.parent_path() / "shortcuts.json";
    WriteLegacyProfile(legacy, {{"file.save", ShortcutBinding::FromKey(EKey::KEY_D, EShortcutModifier::Ctrl)}});
    std::ifstream beforeFile(legacy); const std::string before(std::istreambuf_iterator<char>(beforeFile), {}); beforeFile.close();
    TestEditorShortcutService service;
    ASSERT_TRUE(service.RegisterCommand(Command("file.save", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl))));
    ASSERT_TRUE(service.SetBinding("file.save", ShortcutBinding::FromKey(EKey::KEY_X, EShortcutModifier::Ctrl)));
    ASSERT_TRUE(service.SaveProfile(path));
    TestEditorShortcutService loaded;
    ASSERT_TRUE(loaded.RegisterCommand(Command("file.save", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl))));
    ASSERT_TRUE(loaded.LoadProfile(path, legacy));
    EXPECT_EQ(loaded.GetBindingDisplayText("file.save"), "Ctrl + X");
    EXPECT_FALSE(loaded.SaveProfile(legacy));
    std::ifstream afterFile(legacy); EXPECT_EQ(std::string(std::istreambuf_iterator<char>(afterFile), {}), before);
}

TEST(EditorShortcutPersistenceTests, ConflictingLegacyOverridesKeepOneWinnerAndTheRejectedProposal)
{
    const auto path = TempProfilePath("editor.inputprofile");
    const auto legacy = path.parent_path() / "shortcuts.json";
    WriteLegacyProfile(legacy, {{"first", ShortcutBinding::FromKey(EKey::KEY_C)}, {"second", ShortcutBinding::FromKey(EKey::KEY_C)}});
    TestEditorShortcutService service;
    int first = 0, second = 0;
    auto a = Command("first", ShortcutBinding::FromKey(EKey::KEY_A)); a.execute = [&] { ++first; };
    auto b = Command("second", ShortcutBinding::FromKey(EKey::KEY_B)); b.execute = [&] { ++second; };
    ASSERT_TRUE(service.RegisterCommand(a)); ASSERT_TRUE(service.RegisterCommand(b));
    ASSERT_TRUE(service.LoadProfile(path, legacy));
    EXPECT_EQ(service.GetBindingDisplayText("first"), "C");
    EXPECT_EQ(service.GetBindingDisplayText("second"), "Unassigned");
    EXPECT_FALSE(service.GetProfileDiagnostics().empty());
    const auto profile = service.GetInputUser()->GetProfile();
    ASSERT_EQ(profile.unresolved.size(), 1u);
    EXPECT_EQ(profile.unresolved[0].binding, NLS::Editor::Shortcuts::EditorShortcutAdapter::BindingForCommand("second", 0));
    ASSERT_EQ(profile.unresolved[0].parts.size(), 1u); EXPECT_EQ(profile.unresolved[0].parts[0].path.control, 67);
    service.Press(ShortcutBinding::FromKey(EKey::KEY_C)); EXPECT_EQ(first, 1); EXPECT_EQ(second, 0);
    ASSERT_TRUE(service.SetBinding("second", ShortcutBinding::FromKey(EKey::KEY_D)));
    EXPECT_TRUE(service.GetInputUser()->GetProfile().unresolved.empty());
    ASSERT_TRUE(service.SaveProfile({}));
    TestEditorShortcutService loaded; ASSERT_TRUE(loaded.RegisterCommand(a)); ASSERT_TRUE(loaded.RegisterCommand(b));
    ASSERT_TRUE(loaded.LoadProfile(path, legacy)); EXPECT_EQ(loaded.GetBindingDisplayText("second"), "D");
}

TEST(EditorShortcutPersistenceTests, LegacyCommandsCanExchangeDefaultKeysAtomically)
{
    const auto path = TempProfilePath("editor.inputprofile");
    const auto legacy = path.parent_path() / "shortcuts.json";
    WriteLegacyProfile(legacy, {{"first", ShortcutBinding::FromKey(EKey::KEY_B)}, {"second", ShortcutBinding::FromKey(EKey::KEY_A)}});
    TestEditorShortcutService service;
    ASSERT_TRUE(service.RegisterCommand(Command("first", ShortcutBinding::FromKey(EKey::KEY_A))));
    ASSERT_TRUE(service.RegisterCommand(Command("second", ShortcutBinding::FromKey(EKey::KEY_B))));
    ASSERT_TRUE(service.LoadProfile(path, legacy));
    EXPECT_EQ(service.GetBindingDisplayText("first"), "B"); EXPECT_EQ(service.GetBindingDisplayText("second"), "A");
    EXPECT_TRUE(service.GetProfileDiagnostics().empty());
}

TEST(EditorShortcutPersistenceTests, UnknownLegacyVersionDoesNotCreateANewProfile)
{
    const auto path = TempProfilePath("editor.inputprofile");
    const auto legacy = path.parent_path() / "shortcuts.json";
    { std::ofstream file(legacy); file << R"({"version":999,"bindings":{},"unassigned":[]})"; }
    TestEditorShortcutService service;
    ASSERT_TRUE(service.RegisterCommand(Command("file.save", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl))));
    EXPECT_FALSE(service.LoadProfile(path, legacy));
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(service.GetBindingDisplayText("file.save"), "Ctrl + S");
    EXPECT_FALSE(service.GetProfileDiagnostics().empty());
}

TEST(EditorShortcutPersistenceTests, LegacyNavigationOverrideConfiguresAContinuousAction)
{
    const auto path = TempProfilePath("editor.inputprofile");
    const auto legacy = path.parent_path() / "shortcuts.json";
    WriteLegacyProfile(legacy, {{"scene-view.fly-forward", ShortcutBinding::FromKey(EKey::KEY_I)}});
    TestEditorShortcutService service;
    service.RegisterContext({ShortcutContexts::SceneViewFlyMode, "Fly", 40, "", [] { return true; }});
    auto navigation = Command("scene-view.fly-forward", ShortcutBinding::FromKey(EKey::KEY_W)); navigation.context = ShortcutContexts::SceneViewFlyMode;
    ASSERT_TRUE(service.RegisterCommand(navigation));
    ASSERT_TRUE(service.LoadProfile(path, legacy));
    EXPECT_EQ(service.GetRegistry().FindCommand("scene-view.fly-forward"), nullptr);
    EXPECT_EQ(service.GetBindingDisplayText("scene-view.fly-forward"), "I");
    service.Key(73, true); service.PrepareInput({}); EXPECT_FLOAT_EQ(service.GetNavigationValue().z, 1);
}
