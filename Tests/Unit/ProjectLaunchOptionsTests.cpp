#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <Core/Filesystem/IniFile.h>
#include <Settings/ProjectLaunchOptions.h>

namespace
{
std::filesystem::path MakeProjectFile(const char* p_name, const std::string& p_contents = {})
{
    const auto root = std::filesystem::temp_directory_path() / "NullusProjectLaunchOptionsTests";
    std::filesystem::create_directories(root);
    const auto projectFile = root / p_name;
    std::filesystem::remove(projectFile);
    if (!p_contents.empty())
    {
        std::ofstream file(projectFile);
        file << p_contents;
    }
    return projectFile;
}
}

TEST(ProjectLaunchOptionsTests, MissingKeysKeepDefaultsAndReportAbsence)
{
    const auto projectFile = MakeProjectFile("missing-keys.nullus", "start_scene=Assets/Scenes/Main.scene\n");

    const auto options = NLS::Engine::Settings::ReadProjectLaunchOptions(projectFile);

    EXPECT_TRUE(options.verticalSynchronization);
    EXPECT_TRUE(options.threadedRendering);
    EXPECT_EQ(options.renderDocStartupCaptureFrames, 0);
    EXPECT_FALSE(options.hasVerticalSynchronization);
    EXPECT_FALSE(options.hasThreadedRendering);
    EXPECT_FALSE(options.hasRenderDocStartupCaptureFrames);
}

TEST(ProjectLaunchOptionsTests, RoundTripsEveryLaunchOptionWithoutDroppingOtherKeys)
{
    const auto projectFile = MakeProjectFile("roundtrip.nullus", "start_scene=Assets/Scenes/Main.scene\nx_resolution=1600\n");

    NLS::Engine::Settings::ProjectLaunchOptions written;
    written.verticalSynchronization = false;
    written.threadedRendering = false;
    written.renderDocStartupCaptureFrames = 120;
    ASSERT_TRUE(NLS::Engine::Settings::WriteProjectLaunchOptions(projectFile, written));

    const auto read = NLS::Engine::Settings::ReadProjectLaunchOptions(projectFile);
    EXPECT_FALSE(read.verticalSynchronization);
    EXPECT_FALSE(read.threadedRendering);
    EXPECT_EQ(read.renderDocStartupCaptureFrames, 120);
    EXPECT_TRUE(read.hasVerticalSynchronization);
    EXPECT_TRUE(read.hasThreadedRendering);
    EXPECT_TRUE(read.hasRenderDocStartupCaptureFrames);

    // The launcher must not disturb the other project keys it does not own.
    NLS::Filesystem::IniFile project(projectFile.string());
    EXPECT_EQ(project.GetOrDefault<std::string>("start_scene", ""), "Assets/Scenes/Main.scene");
    EXPECT_EQ(project.GetOrDefault<int>("x_resolution", 0), 1600);
}

TEST(ProjectLaunchOptionsTests, NegativeCaptureFrameCountsClampToZero)
{
    const auto projectFile = MakeProjectFile("clamped.nullus", "vsync=true\n");

    NLS::Engine::Settings::ProjectLaunchOptions written;
    written.renderDocStartupCaptureFrames = -5;
    ASSERT_TRUE(NLS::Engine::Settings::WriteProjectLaunchOptions(projectFile, written));

    const auto read = NLS::Engine::Settings::ReadProjectLaunchOptions(projectFile);
    EXPECT_EQ(read.renderDocStartupCaptureFrames, 0);
}

TEST(ProjectLaunchOptionsTests, MissingFileKeepsDefaultsAndReportsNoError)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusProjectLaunchOptionsTests";
    const auto missing = root / "does-not-exist.nullus";
    std::filesystem::remove(missing);

    std::string error;
    const auto options = NLS::Engine::Settings::ReadProjectLaunchOptions(missing, &error);

    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(options.verticalSynchronization);
    EXPECT_FALSE(options.hasVerticalSynchronization);
}

TEST(ProjectLaunchOptionsTests, WriteCreatesTheProjectFileWhenAbsent)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusProjectLaunchOptionsTests";
    std::filesystem::create_directories(root);
    const auto projectFile = root / "created.nullus";
    std::filesystem::remove(projectFile);

    NLS::Engine::Settings::ProjectLaunchOptions written;
    written.threadedRendering = false;
    ASSERT_TRUE(NLS::Engine::Settings::WriteProjectLaunchOptions(projectFile, written));

    ASSERT_TRUE(std::filesystem::exists(projectFile));
    const auto read = NLS::Engine::Settings::ReadProjectLaunchOptions(projectFile);
    EXPECT_FALSE(read.threadedRendering);
    EXPECT_TRUE(read.hasThreadedRendering);
}
