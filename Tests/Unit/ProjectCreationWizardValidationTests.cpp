#include <gtest/gtest.h>

#include <filesystem>

#include "Core/ProjectCreationWizard.h"

TEST(ProjectCreationWizardValidationTests, RequiresSelectedEditorVersionWhenCreatingProject)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusProjectCreationValidationTests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    NLS::ProjectCreationConfig config;
    config.projectName = "MyProject";
    config.projectLocation = root.string();

    const auto result = NLS::ValidateProjectCreationConfig(config);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.messageKey, NLS::LauncherTextKey::WizardEditorVersionRequired);
    EXPECT_TRUE(result.requiresModalPrompt);
}
