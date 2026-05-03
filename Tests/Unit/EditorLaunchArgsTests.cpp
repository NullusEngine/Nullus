#include <gtest/gtest.h>

#include <vector>

#include "Core/EditorLaunchArgs.h"
#include "Rendering/RHI/Backends/DX12/DX12Device.h"

namespace
{
    char** MutableArgv(const std::initializer_list<const char*> args, std::vector<std::string>& storage)
    {
        storage.assign(args.begin(), args.end());
        static std::vector<char*> argv;
        argv.clear();
        for (std::string& value : storage)
            argv.push_back(value.data());
        return argv.data();
    }
}

TEST(EditorLaunchArgsTests, DefaultsToDebugValidationForDiagnosticSafety)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.enableRhiDebugValidation);
}

TEST(EditorLaunchArgsTests, PerformanceModeDisablesRhiDebugValidation)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "--editor-performance-mode", "TestProject.nullus"}, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.enablePerformanceMode);
    EXPECT_FALSE(parsed.enableRhiDebugValidation);
}

TEST(EditorLaunchArgsTests, ExplicitNoDebugValidationFlagDisablesRhiDebugValidation)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "--no-rhi-debug-validation", "TestProject.nullus"}, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_FALSE(parsed.enableRhiDebugValidation);
}

#if defined(_WIN32)
TEST(EditorLaunchArgsTests, DefaultDx12DeviceCreationEnablesDredDiagnostics)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);
    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(parsed.enableRhiDebugValidation);

    EXPECT_TRUE(resources.IsValid());
    EXPECT_TRUE(resources.dredDiagnosticsEnabled);
}

TEST(EditorLaunchArgsTests, PerformanceModeDx12DeviceCreationSkipsDredDiagnostics)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "--editor-performance-mode", "TestProject.nullus"}, storage);
    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(parsed.enableRhiDebugValidation);

    EXPECT_TRUE(resources.IsValid());
    EXPECT_FALSE(resources.dredDiagnosticsEnabled);
}
#endif
