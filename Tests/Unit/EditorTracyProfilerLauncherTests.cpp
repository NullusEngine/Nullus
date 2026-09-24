#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include "Debug/TracyProfilerLauncher.h"

namespace
{
#if defined(_WIN32)
constexpr const char* kProfilerExecutableName = "tracy-profiler.exe";
#else
constexpr const char* kProfilerExecutableName = "tracy-profiler";
#endif

class TemporaryTracyLayout final
{
public:
    TemporaryTracyLayout()
        : root(std::filesystem::temp_directory_path() /
            ("NullusTracyLauncher_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(root);
    }

    ~TemporaryTracyLayout()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    std::filesystem::path CreateFile(const std::filesystem::path& p_relativePath) const
    {
        const auto path = root / p_relativePath;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "stub";
        return path;
    }

    std::filesystem::path root;
};

std::filesystem::path Normalized(const std::filesystem::path& p_path)
{
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(p_path, error);
    return error ? p_path : canonical;
}

NLS::Editor::Debug::TracyProfilerSearchOptions MakeOptions(
    const std::filesystem::path& p_startDirectory,
    std::optional<std::filesystem::path> p_toolsOverride = std::nullopt)
{
    NLS::Editor::Debug::TracyProfilerSearchOptions options;
    options.startDirectory = p_startDirectory;
    options.toolsOverride = std::move(p_toolsOverride);
    return options;
}
}

TEST(EditorTracyProfilerLauncher, PrefersToolsOverrideOverCheckoutLayout)
{
    TemporaryTracyLayout overrideLayout;
    TemporaryTracyLayout checkoutLayout;
    const auto overrideExecutable = overrideLayout.CreateFile(kProfilerExecutableName);
    checkoutLayout.CreateFile(
        std::filesystem::path("Tools") / "Tracy" / "0.13.3" / kProfilerExecutableName);

    const auto resolution = NLS::Editor::Debug::ResolveTracyProfiler(MakeOptions(
        checkoutLayout.root / "App" / "Win64_RelWithDebInfo_Runtime_Shared",
        overrideExecutable));

    ASSERT_TRUE(resolution.IsValid());
    EXPECT_EQ(resolution.executablePath, Normalized(overrideExecutable));
}

TEST(EditorTracyProfilerLauncher, ResolvesNewestVersionedToolsFromRepositoryLayout)
{
    TemporaryTracyLayout layout;
    layout.CreateFile(std::filesystem::path("Tools") / "Tracy" / "0.13.2" / kProfilerExecutableName);
    const auto newestExecutable = layout.CreateFile(
        std::filesystem::path("Tools") / "Tracy" / "0.13.3" / kProfilerExecutableName);

    const auto resolution = NLS::Editor::Debug::ResolveTracyProfiler(
        MakeOptions(layout.root / "App" / "Win64_RelWithDebInfo_Runtime_Shared"));

    ASSERT_TRUE(resolution.IsValid());
    EXPECT_EQ(resolution.executablePath, Normalized(newestExecutable));
    EXPECT_EQ(resolution.repositoryRoot, Normalized(layout.root));
    EXPECT_EQ(resolution.toolsRoot, Normalized(newestExecutable.parent_path()));
}

TEST(EditorTracyProfilerLauncher, ResolvesToolsDirectoryOverride)
{
    TemporaryTracyLayout layout;
    const auto executable = layout.CreateFile(std::filesystem::path("tracy") / kProfilerExecutableName);

    const auto resolution = NLS::Editor::Debug::ResolveTracyProfiler(
        MakeOptions(layout.root / "elsewhere", executable.parent_path()));

    ASSERT_TRUE(resolution.IsValid());
    EXPECT_EQ(resolution.executablePath, Normalized(executable));
}

TEST(EditorTracyProfilerLauncher, PrefersProfilerNextToTheEditor)
{
    TemporaryTracyLayout layout;
    const auto startDirectory = layout.root / "App" / "Win64_RelWithDebInfo_Runtime_Shared";
    const auto localExecutable = layout.CreateFile(
        std::filesystem::path("App") / "Win64_RelWithDebInfo_Runtime_Shared" / kProfilerExecutableName);
    layout.CreateFile(std::filesystem::path("Tools") / "Tracy" / "0.13.3" / kProfilerExecutableName);

    const auto resolution = NLS::Editor::Debug::ResolveTracyProfiler(MakeOptions(startDirectory));

    ASSERT_TRUE(resolution.IsValid());
    EXPECT_EQ(resolution.executablePath, Normalized(localExecutable));
}

TEST(EditorTracyProfilerLauncher, ResolvesVersionedToolsDeployedNextToTheEditor)
{
    TemporaryTracyLayout layout;
    const auto startDirectory = layout.root / "Win64_Debug_Runtime_Shared";
    const auto deployedExecutable = layout.CreateFile(
        std::filesystem::path("Win64_Debug_Runtime_Shared") / "Tracy" / "0.13.3" / kProfilerExecutableName);

    const auto resolution = NLS::Editor::Debug::ResolveTracyProfiler(MakeOptions(startDirectory));

    ASSERT_TRUE(resolution.IsValid());
    EXPECT_EQ(resolution.executablePath, Normalized(deployedExecutable));
}

TEST(EditorTracyProfilerLauncher, ReportsMissingProfilerWithoutCandidates)
{
    TemporaryTracyLayout layout;

    const auto resolution = NLS::Editor::Debug::ResolveTracyProfiler(
        MakeOptions(layout.root / "App" / "Win64_RelWithDebInfo_Runtime_Shared"));

    EXPECT_FALSE(resolution.IsValid());
    EXPECT_TRUE(resolution.repositoryRoot.empty());
    EXPECT_TRUE(NLS::Editor::Debug::ResolveTracyCaptureDirectory(resolution).empty());
}

TEST(EditorTracyProfilerLauncher, ResolvesCaptureDirectoryForRepositoryCheckout)
{
    TemporaryTracyLayout layout;
    layout.CreateFile(std::filesystem::path("Tools") / "Tracy" / "0.13.3" / kProfilerExecutableName);
    const auto captureDirectory = layout.root / "Build" / "Tracy";
    std::filesystem::create_directories(captureDirectory);

    const auto resolution = NLS::Editor::Debug::ResolveTracyProfiler(
        MakeOptions(layout.root / "App" / "Win64_RelWithDebInfo_Runtime_Shared"));
    ASSERT_TRUE(resolution.IsValid());

    const auto resolvedCaptureDirectory = NLS::Editor::Debug::ResolveTracyCaptureDirectory(resolution);
    EXPECT_EQ(resolvedCaptureDirectory.filename(), std::filesystem::path("Tracy"));
    EXPECT_TRUE(std::filesystem::is_directory(resolvedCaptureDirectory));
}
