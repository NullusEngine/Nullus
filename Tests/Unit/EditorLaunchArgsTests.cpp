#include <gtest/gtest.h>

#include <vector>

#include "Core/EditorFrameLatency.h"
#include "Core/EditorLaunchArgs.h"
#include "Rendering/RHI/Backends/DX12/DX12Device.h"
#include "Rendering/Settings/DriverSettings.h"

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

TEST(EditorLaunchArgsTests, DriverSettingsDebugModeFollowsBuildConfiguration)
{
    NLS::Render::Settings::DriverSettings settings;
#ifdef _DEBUG
    EXPECT_TRUE(settings.debugMode);
#else
    EXPECT_FALSE(settings.debugMode);
#endif
}

TEST(EditorLaunchArgsTests, RejectsRemovedDebugCliFlags)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "--log-editor-fps", "TestProject.nullus"}, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_TRUE(parsed.hasError);
}

TEST(EditorLaunchArgsTests, ParsesSceneViewValidationCameraDirective)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-validation-scene-camera",
        "1,2,3;10,20,30",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.hasDiagnosticsOverride);
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationSceneCamera, "1,2,3;10,20,30");
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, ParsesEditorValidationViewAndCameraInputDiagnostics)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-validation-focus-view",
        "scene",
        "--editor-validation-exclusive-view",
        "game",
        "--editor-validation-select-gameobject",
        "Validation Cube",
        "--editor-log-render-draw-path",
        "--editor-log-scene-camera-input",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.hasDiagnosticsOverride);
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationFocusView, "scene");
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationExclusiveView, "game");
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationSelectGameObject, "Validation Cube");
    EXPECT_TRUE(parsed.diagnosticsSettings.logRenderDrawPath);
    EXPECT_TRUE(parsed.diagnosticsSettings.dx12LogFrameFlow);
    EXPECT_TRUE(parsed.diagnosticsSettings.editorLogSceneCameraInput);
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, ParsesSceneViewReadbackValidationOutputs)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-validation-scene-readback-output",
        "Build/SceneViewReadback/output.png",
        "--editor-validation-scene-readback-summary",
        "Build/SceneViewReadback/output.txt",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.hasDiagnosticsOverride);
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationSceneReadbackOutput, "Build/SceneViewReadback/output.png");
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationSceneReadbackSummary, "Build/SceneViewReadback/output.txt");
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
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

    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);

    EXPECT_TRUE(resources.IsValid());
    EXPECT_FALSE(resources.dredDiagnosticsEnabled);
}

TEST(EditorLaunchArgsTests, ExplicitDebugValidationDx12DeviceCreationEnablesDredDiagnostics)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);

    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(true);

    EXPECT_TRUE(resources.IsValid());
    EXPECT_TRUE(resources.dredDiagnosticsEnabled);
}

TEST(EditorLaunchArgsTests, DisabledDebugValidationDx12DeviceCreationSkipsDredDiagnostics)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);

    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);

    EXPECT_TRUE(resources.IsValid());
    EXPECT_FALSE(resources.dredDiagnosticsEnabled);
}
#endif
