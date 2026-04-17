#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "Core/LauncherSettings.h"

namespace
{
std::filesystem::path MakeExecutableFile(const std::filesystem::path& root, const std::string& name)
{
    const auto executable = root / name;
    std::ofstream exe(executable, std::ios::binary);
    exe << "stub";
    return executable;
}
}

TEST(LauncherSettingsTests, PersistsAndReloadsMultipleEngineExecutablePaths)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusLauncherSettingsTests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto editorA = MakeExecutableFile(root, "EditorA.exe");
    const auto editorB = MakeExecutableFile(root, "EditorB.exe");

    const auto settingsPath = root / "launcher.ini";
    NLS::LauncherSettings settings(settingsPath);
    EXPECT_TRUE(settings.AddEngineExecutablePath(editorA));
    EXPECT_TRUE(settings.AddEngineExecutablePath(editorB));
    EXPECT_TRUE(settings.SetDefaultEngineExecutablePath(editorB));
    EXPECT_TRUE(settings.Save());

    NLS::LauncherSettings reloaded(settingsPath);
    EXPECT_TRUE(reloaded.Load());
    ASSERT_EQ(reloaded.GetEngineInstallations().size(), 2u);
    EXPECT_EQ(reloaded.GetEngineInstallations()[0].executablePath, editorA.string());
    EXPECT_EQ(reloaded.GetEngineInstallations()[1].executablePath, editorB.string());
    EXPECT_EQ(reloaded.GetDefaultEngineExecutablePath(), editorB.string());
    EXPECT_TRUE(reloaded.HasValidDefaultEngineExecutablePath());
}

TEST(LauncherSettingsTests, RemovesInstallAndFallsBackToFirstRemainingDefault)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusLauncherSettingsTests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto editorA = MakeExecutableFile(root, "EditorA.exe");
    const auto editorB = MakeExecutableFile(root, "EditorB.exe");

    NLS::LauncherSettings settings(root / "launcher.ini");
    ASSERT_TRUE(settings.AddEngineExecutablePath(editorA));
    ASSERT_TRUE(settings.AddEngineExecutablePath(editorB));
    ASSERT_TRUE(settings.SetDefaultEngineExecutablePath(editorB));

    EXPECT_TRUE(settings.RemoveEngineExecutablePath(editorB));
    ASSERT_EQ(settings.GetEngineInstallations().size(), 1u);
    EXPECT_EQ(settings.GetDefaultEngineExecutablePath(), editorA.string());
}

TEST(LauncherSettingsTests, RejectsMissingEngineExecutablePath)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusLauncherSettingsTests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    NLS::LauncherSettings settings(root / "launcher.ini");
    EXPECT_FALSE(settings.AddEngineExecutablePath(root / "MissingEditor.exe"));
    EXPECT_FALSE(settings.HasValidDefaultEngineExecutablePath());
}

#ifdef _WIN32
TEST(LauncherSettingsTests, RejectsNonExecutableExtensionOnWindows)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusLauncherSettingsTests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto invalidExecutable = root / "Editor.txt";
    {
        std::ofstream file(invalidExecutable, std::ios::binary);
        file << "stub";
    }

    NLS::LauncherSettings settings(root / "launcher.ini");
    EXPECT_FALSE(settings.AddEngineExecutablePath(invalidExecutable));
    EXPECT_FALSE(settings.HasValidDefaultEngineExecutablePath());
}
#endif
