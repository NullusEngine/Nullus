#include <gtest/gtest.h>

#include <vector>

#include "Core/EditorFrameLatency.h"
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

TEST(EditorLaunchArgsTests, DefaultsToPerformanceFriendlyRhiValidation)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_FALSE(parsed.enableRhiDebugValidation);
}

TEST(EditorLaunchArgsTests, ExplicitDebugValidationFlagEnablesRhiDebugValidation)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "--rhi-debug-validation", "TestProject.nullus"}, storage);

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

TEST(EditorLaunchArgsTests, ScenePickingDiagnosticsCanBeEnabledWithoutDx12FrameFlowLogging)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "--editor-log-scene-picking", "TestProject.nullus"}, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.diagnosticsSettings.editorLogScenePicking);
    EXPECT_FALSE(parsed.diagnosticsSettings.dx12LogFrameFlow);
}

TEST(EditorLaunchArgsTests, EditorFpsDiagnosticsCanBeEnabled)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "--log-editor-fps", "TestProject.nullus"}, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.diagnosticsSettings.logEditorFps);
}

TEST(EditorLaunchArgsTests, EditorThreadedRenderingUsesFramesInFlightSlotsForThroughput)
{
    EXPECT_EQ(NLS::Editor::Core::ResolveEditorThreadedFrameSlotCount(0u), 1u);
    EXPECT_EQ(NLS::Editor::Core::ResolveEditorThreadedFrameSlotCount(2u), 2u);
    EXPECT_EQ(NLS::Editor::Core::ResolveEditorThreadedFrameSlotCount(3u), 3u);
}

#if defined(_WIN32)
TEST(EditorLaunchArgsTests, DefaultDx12DeviceCreationSkipsDredDiagnostics)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);
    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(parsed.enableRhiDebugValidation);

    EXPECT_TRUE(resources.IsValid());
    EXPECT_FALSE(resources.dredDiagnosticsEnabled);
}

TEST(EditorLaunchArgsTests, ExplicitDebugValidationDx12DeviceCreationEnablesDredDiagnostics)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "--rhi-debug-validation", "TestProject.nullus"}, storage);
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
