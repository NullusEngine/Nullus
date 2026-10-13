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
    EXPECT_EQ(options.rhiThreadMode, NLS::Render::Settings::RhiThreadModePreference::BackendDefault);
    EXPECT_EQ(options.renderDocStartupCaptureFrames, 0);
    EXPECT_FALSE(options.hasVerticalSynchronization);
    EXPECT_FALSE(options.hasRhiThreadMode);
    EXPECT_FALSE(options.hasRenderDocStartupCaptureFrames);
}

TEST(ProjectLaunchOptionsTests, RoundTripsEveryLaunchOptionWithoutDroppingOtherKeys)
{
    const auto projectFile = MakeProjectFile("roundtrip.nullus", "start_scene=Assets/Scenes/Main.scene\nx_resolution=1600\n");

    NLS::Engine::Settings::ProjectLaunchOptions written;
    written.verticalSynchronization = false;
    written.rhiThreadMode = NLS::Render::Settings::RhiThreadModePreference::ForceTasks;
    written.renderDocStartupCaptureFrames = 120;
    ASSERT_TRUE(NLS::Engine::Settings::WriteProjectLaunchOptions(projectFile, written));

    const auto read = NLS::Engine::Settings::ReadProjectLaunchOptions(projectFile);
    EXPECT_FALSE(read.verticalSynchronization);
    EXPECT_EQ(read.rhiThreadMode, written.rhiThreadMode);
    EXPECT_EQ(read.renderDocStartupCaptureFrames, 120);
    EXPECT_TRUE(read.hasVerticalSynchronization);
    EXPECT_TRUE(read.hasRhiThreadMode);
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
    written.rhiThreadMode = NLS::Render::Settings::RhiThreadModePreference::ForceDedicatedThread;
    ASSERT_TRUE(NLS::Engine::Settings::WriteProjectLaunchOptions(projectFile, written));

    ASSERT_TRUE(std::filesystem::exists(projectFile));
    const auto read = NLS::Engine::Settings::ReadProjectLaunchOptions(projectFile);
    EXPECT_EQ(read.rhiThreadMode, written.rhiThreadMode);
    EXPECT_TRUE(read.hasRhiThreadMode);
}

TEST(ProjectLaunchOptionsTests, AllRhiModesRoundTripAsCanonicalSettings)
{
    using NLS::Render::Settings::RhiThreadModePreference;
    const auto projectFile = MakeProjectFile("rhi-modes.nullus");
    for (const auto mode : {RhiThreadModePreference::BackendDefault, RhiThreadModePreference::ForceNone,
        RhiThreadModePreference::ForceDedicatedThread, RhiThreadModePreference::ForceTasks})
    {
        NLS::Engine::Settings::ProjectLaunchOptions options;
        options.rhiThreadMode = mode;
        ASSERT_TRUE(NLS::Engine::Settings::WriteProjectLaunchOptions(projectFile, options));
        std::string error;
        const auto loaded = NLS::Engine::Settings::ReadProjectLaunchOptions(projectFile, &error);
        EXPECT_TRUE(error.empty());
        EXPECT_TRUE(loaded.hasRhiThreadMode);
        EXPECT_EQ(loaded.rhiThreadMode, mode);
        NLS::Filesystem::IniFile file(projectFile.string());
        EXPECT_EQ(file.Get<std::string>(NLS::Engine::Settings::ProjectLaunchOptionKeys::RhiThreadMode),
            NLS::Render::Settings::ToConfigString(mode));
    }
}

TEST(ProjectLaunchOptionsTests, InvalidRhiModeIsReportedWithoutRewritingTheFile)
{
    const auto projectFile = MakeProjectFile("invalid-rhi-mode.nullus", "rhi_thread_mode=invalid\nvsync=false\n");
    std::string error;
    const auto loaded = NLS::Engine::Settings::ReadProjectLaunchOptions(projectFile, &error);
    EXPECT_EQ(error, "Invalid rhi_thread_mode: invalid");
    EXPECT_FALSE(loaded.hasRhiThreadMode);
    NLS::Engine::Settings::ProjectLaunchOptions invalid;
    invalid.rhiThreadMode = static_cast<NLS::Render::Settings::RhiThreadModePreference>(255u);
    EXPECT_FALSE(NLS::Engine::Settings::WriteProjectLaunchOptions(projectFile, invalid, &error));
    EXPECT_FALSE(error.empty());
    NLS::Filesystem::IniFile file(projectFile.string());
    EXPECT_EQ(file.Get<std::string>("rhi_thread_mode"), "invalid");
    EXPECT_FALSE(file.Get<bool>("vsync"));
}

TEST(ProjectLaunchOptionsTests, ObsoleteThreadedRenderingToggleIsIgnoredAndRemovedOnSave)
{
    const auto projectFile = MakeProjectFile("obsolete-rendering-toggle.nullus",
        "enable_threaded_rendering=false\nrhi_thread_mode=tasks\nstart_scene=Assets/Scenes/Main.scene\n");
    const auto loaded = NLS::Engine::Settings::ReadProjectLaunchOptions(projectFile);
    EXPECT_EQ(loaded.rhiThreadMode, NLS::Render::Settings::RhiThreadModePreference::ForceTasks);
    ASSERT_TRUE(NLS::Engine::Settings::WriteProjectLaunchOptions(projectFile, loaded));
    NLS::Filesystem::IniFile file(projectFile.string());
    EXPECT_FALSE(file.IsKeyExisting("enable_threaded_rendering"));
    EXPECT_EQ(file.Get<std::string>("start_scene"), "Assets/Scenes/Main.scene");
}
