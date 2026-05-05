#include <gtest/gtest.h>

#include "Panels/ShortcutSettingsPanel.h"

using namespace NLS::Editor;
using namespace NLS::Editor::Shortcuts;

namespace
{
ShortcutCommand Command(std::string id, std::string displayName, ShortcutContextId context)
{
    ShortcutCommand command;
    command.id = std::move(id);
    command.displayName = std::move(displayName);
    command.category = "Edit";
    command.context = std::move(context);
    return command;
}
}

TEST(ShortcutSettingsPanelTests, AppendsContextToDuplicateCommandDisplayNames)
{
    const auto sceneDelete = Command("edit.delete-selected-actor", "Delete Selected Actor", ShortcutContexts::SceneView);
    const auto hierarchyDelete = Command("edit.delete-selected-actor-hierarchy", "Delete Selected Actor", ShortcutContexts::Hierarchy);
    const std::vector<const ShortcutCommand*> commands { &sceneDelete, &hierarchyDelete };

    EXPECT_EQ(
        Panels::ShortcutSettingsPanel::GetCommandListDisplayName(sceneDelete, commands),
        "Delete Selected Actor (Scene View)");
    EXPECT_EQ(
        Panels::ShortcutSettingsPanel::GetCommandListDisplayName(hierarchyDelete, commands),
        "Delete Selected Actor (Hierarchy)");
}

TEST(ShortcutSettingsPanelTests, LeavesUniqueCommandDisplayNamesUnchanged)
{
    const auto saveScene = Command("file.save-scene", "Save Scene", ShortcutContexts::Global);
    const std::vector<const ShortcutCommand*> commands { &saveScene };

    EXPECT_EQ(
        Panels::ShortcutSettingsPanel::GetCommandListDisplayName(saveScene, commands),
        "Save Scene");
}
