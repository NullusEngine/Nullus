#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "Core/LauncherProjectMetadata.h"
#include "Core/LauncherSettings.h"

namespace
{
std::filesystem::path MakeProjectRoot(const std::filesystem::path& root, const std::string& projectName)
{
    const auto projectRoot = root / projectName;
    std::filesystem::create_directories(projectRoot);

    std::ofstream projectFile(projectRoot / (projectName + ".nullus"));
    projectFile << "graphics_backend=vulkan\n";
    projectFile.close();

    return projectRoot;
}
}

TEST(LauncherProjectMetadataTests, PersistsLastEditorExecutableInsideProjectFile)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusLauncherProjectMetadataTests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto editorPath = root / "Editor.exe";
    std::ofstream editorFile(editorPath);
    editorFile << "editor";
    editorFile.close();

    const auto projectRoot = MakeProjectRoot(root, "BoundEditorProject");

    ASSERT_TRUE(NLS::WriteProjectLastEditorExecutable(projectRoot, editorPath.string()));
    EXPECT_EQ(NLS::ReadProjectLastEditorExecutable(projectRoot), editorPath.string());
}

TEST(LauncherProjectMetadataTests, DescribesBoundEditorVersionFromStoredExecutablePath)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusLauncherProjectMetadataVersionTests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto editorPath = root / "Editor.exe";
    std::ofstream editorFile(editorPath);
    editorFile << "editor";
    editorFile.close();

    const auto projectRoot = MakeProjectRoot(root, "ProjectVersionBinding");
    ASSERT_TRUE(NLS::WriteProjectLastEditorExecutable(projectRoot, editorPath.string()));

    EXPECT_EQ(
        NLS::DescribeProjectLastEditorVersion(projectRoot),
        NLS::LauncherSettings::DescribeEngineVersion(editorPath));
}
