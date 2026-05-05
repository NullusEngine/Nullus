#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "Shortcuts/EditorShortcutProfile.h"
#include "Shortcuts/EditorShortcutService.h"

using namespace NLS::Editor::Shortcuts;
using namespace NLS::Windowing::Inputs;

namespace
{
std::filesystem::path TempProfilePath(const std::string& p_name)
{
    const auto dir = std::filesystem::temp_directory_path() / "NullusShortcutTests";
    std::filesystem::create_directories(dir);
    return dir / p_name;
}

ShortcutCommand Command(std::string id, ShortcutBinding binding)
{
    ShortcutCommand command;
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
    const auto path = TempProfilePath("overrides.json");
    std::filesystem::remove(path);

    EditorShortcutProfile profile;
    profile.SetBindingOverride("file.save", ShortcutBinding::FromKey(EKey::KEY_D, EShortcutModifier::Ctrl));

    ASSERT_TRUE(profile.Save(path));

    EditorShortcutProfile loaded;
    ASSERT_TRUE(loaded.Load(path));

    EXPECT_EQ(
        loaded.GetBindingOverride("file.save"),
        ShortcutBinding::FromKey(EKey::KEY_D, EShortcutModifier::Ctrl));
}

TEST(EditorShortcutPersistenceTests, PersistsUnassignedCommands)
{
    const auto path = TempProfilePath("unassigned.json");
    std::filesystem::remove(path);

    EditorShortcutProfile profile;
    profile.SetCommandUnassigned("file.save");
    ASSERT_TRUE(profile.Save(path));

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

    EditorShortcutProfile profile;
    profile.SetBindingOverride("file.save", ShortcutBinding::FromKey(EKey::KEY_D, EShortcutModifier::Ctrl));
    profile.SetCommandUnassigned("file.new");
    ASSERT_TRUE(profile.Save(path));

    EditorShortcutService service;
    service.RegisterCommand(Command("file.save", ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl)));
    service.RegisterCommand(Command("file.new", ShortcutBinding::FromKey(EKey::KEY_N, EShortcutModifier::Ctrl)));

    EXPECT_TRUE(service.LoadProfile(path));

    EXPECT_EQ(service.GetBinding("file.save"), ShortcutBinding::FromKey(EKey::KEY_D, EShortcutModifier::Ctrl));
    EXPECT_FALSE(service.GetBinding("file.new").assigned);
}

TEST(EditorShortcutPersistenceTests, ServiceAssignClearAndResetWorkflow)
{
    EditorShortcutService service;
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
    EditorShortcutService service;
    auto command = Command("editor.required", ShortcutBinding::FromKey(EKey::KEY_F5));
    command.requiredBinding = true;
    service.RegisterCommand(command);

    ASSERT_NE(service.GetRegistry().FindCommand("editor.required"), nullptr);
    EXPECT_TRUE(service.GetRegistry().FindCommand("editor.required")->requiredBinding);

    EXPECT_FALSE(service.ClearBinding("editor.required"));
    EXPECT_EQ(service.GetBindingDisplayText("editor.required"), "F5");
}
