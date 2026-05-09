#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "Core/Filesystem/IniFile.h"
#include "Guid.h"

TEST(EditorProjectSettingsTests, NullusProjectFileCanPersistLastOpenedScene)
{
    NLS::Filesystem::IniFile projectSettings("unused.nullus");

    ASSERT_TRUE(projectSettings.Add<std::string>("last_opened_scene", "Assets/Scenes/New.scene"));
    EXPECT_EQ(projectSettings.Get<std::string>("last_opened_scene"), "Assets/Scenes/New.scene");

    ASSERT_TRUE(projectSettings.Set<std::string>("last_opened_scene", "Assets/Scenes/Other.scene"));
    EXPECT_EQ(projectSettings.Get<std::string>("last_opened_scene"), "Assets/Scenes/Other.scene");
}

TEST(EditorProjectSettingsTests, NullusProjectFilePreservesSpacesInPersistedScenePath)
{
    const auto settingsPath =
        std::filesystem::temp_directory_path() /
        ("nullus_project_settings_" + NLS::Guid::New().ToString() + ".nullus");

    {
        std::ofstream file(settingsPath);
        file << "last_opened_scene=Scenes/New Scene.scene\n";
    }

    NLS::Filesystem::IniFile projectSettings(settingsPath.string());

    EXPECT_EQ(projectSettings.Get<std::string>("last_opened_scene"), "Scenes/New Scene.scene");

    std::error_code error;
    std::filesystem::remove(settingsPath, error);
}

TEST(EditorProjectSettingsTests, EditorSwapchainUsesProjectVsyncSetting)
{
    const std::filesystem::path contextSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Context.cpp";

    std::ifstream stream(contextSourcePath, std::ios::binary);
    const std::string contextSource{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>() };

    ASSERT_FALSE(contextSource.empty());
    EXPECT_NE(contextSource.find("projectSettings.GetOrDefault<bool>(\"vsync\", true)"), std::string::npos);
    EXPECT_EQ(contextSource.find("static_cast<uint32_t>(initialFramebufferSize.y),\n        true)"), std::string::npos);
}
