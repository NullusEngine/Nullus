#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
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

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
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
        "--editor-validation-open-frame-info",
        "--editor-validation-open-profiler",
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
    EXPECT_TRUE(parsed.diagnosticsSettings.editorValidationOpenFrameInfo);
    EXPECT_TRUE(parsed.diagnosticsSettings.editorValidationOpenProfiler);
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationSelectGameObject, "Validation Cube");
    EXPECT_TRUE(parsed.diagnosticsSettings.logRenderDrawPath);
    EXPECT_TRUE(parsed.diagnosticsSettings.dx12LogFrameFlow);
    EXPECT_TRUE(parsed.diagnosticsSettings.editorLogSceneCameraInput);
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, ParsesEditorValidationTimelineTraceFrames)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-validation-trace-frames",
        "300",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.hasDiagnosticsOverride);
    EXPECT_TRUE(parsed.diagnosticsSettings.editorValidationOpenProfiler);
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationTimelineTraceFrames, 300u);
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, ParsesHZBOcclusionComparisonValidationDirectives)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-validation-disable-hzb-occlusion",
        "--editor-validation-occlusion-stack",
        "6",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.hasDiagnosticsOverride);
    EXPECT_TRUE(parsed.diagnosticsSettings.editorValidationDisableHZBOcclusion);
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationOcclusionStackCount, 6u);
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, HZBOcclusionValidationStackUsesLargeOccluderAndSmallerTargets)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Editor.cpp");

    const auto functionStart = source.find("void CreateValidationOcclusionStack");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void PublishReflectionDiagnosticsToLog", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("kOccluderScale = 4.0f"), std::string::npos);
    EXPECT_NE(body.find("kTargetScale = 0.75f"), std::string::npos);
    EXPECT_NE(body.find("kTargetStartDistance = 10.0f"), std::string::npos);
    EXPECT_NE(body.find("camera.GetPosition()"), std::string::npos);
    EXPECT_NE(body.find("camera.transform->GetWorldForward()"), std::string::npos);
    EXPECT_NE(body.find("camera.GetRotation() * Maths::Vector3::Right"), std::string::npos);
    EXPECT_NE(body.find("camera.GetRotation() * Maths::Vector3::Up"), std::string::npos);
    EXPECT_NE(body.find("const bool isOccluder = index == 0u"), std::string::npos);
    EXPECT_NE(body.find("const float distance = isOccluder"), std::string::npos);
    EXPECT_NE(body.find("cameraForward * distance"), std::string::npos);
    EXPECT_NE(body.find("cameraRight * lane"), std::string::npos);
    EXPECT_NE(body.find("cameraUp * row"), std::string::npos);
    EXPECT_NE(body.find("const float scale = isOccluder ? kOccluderScale : kTargetScale"), std::string::npos);
    EXPECT_EQ(body.find("{\n            0.0f,\n            0.0f,\n            -distance"), std::string::npos);
}

TEST(EditorLaunchArgsTests, SceneViewValidationReadbackWaitsForHZBHistoryWarmup)
{
    const auto header = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/SceneView.h");
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/SceneView.cpp");

    EXPECT_NE(header.find("m_validationReadbackWarmupFrames"), std::string::npos);

    const auto functionStart = source.find("void Editor::Panels::SceneView::TryWriteValidationReadback");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void Editor::Panels::SceneView::DrawViewportOverlay", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("kValidationReadbackWarmupFrames = 4u"), std::string::npos);
    EXPECT_NE(body.find("m_validationReadbackWarmupFrames < kValidationReadbackWarmupFrames"), std::string::npos);
    EXPECT_NE(body.find("++m_validationReadbackWarmupFrames"), std::string::npos);
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
    EXPECT_EQ(NLS::Editor::Core::ResolveEditorThreadedFrameSlotCount(2u), 3u);
    EXPECT_EQ(NLS::Editor::Core::ResolveEditorThreadedFrameSlotCount(3u), 4u);
    EXPECT_EQ(NLS::Editor::Core::ResolveEditorThreadedPublishRetirementWaitMs(), 8u);
}

#if defined(_WIN32)
TEST(EditorLaunchArgsTests, DefaultDx12DeviceCreationEnablesDredDiagnostics)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);

    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);

    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable or Shader Model 6 unsupported on this test machine";

    EXPECT_TRUE(resources.IsValid());
    EXPECT_TRUE(resources.shaderModel6Supported);
    EXPECT_GE(resources.confirmedShaderModel, static_cast<unsigned int>(D3D_SHADER_MODEL_6_0));
    EXPECT_NE(resources.shaderModelDiagnostics.find("Shader Model"), std::string::npos);
    EXPECT_TRUE(resources.dredDiagnosticsEnabled);
}

TEST(EditorLaunchArgsTests, ExplicitDebugValidationDx12DeviceCreationEnablesDredDiagnostics)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);

    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(true);

    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable or Shader Model 6 unsupported on this test machine";

    EXPECT_TRUE(resources.IsValid());
    EXPECT_TRUE(resources.shaderModel6Supported);
    EXPECT_GE(resources.confirmedShaderModel, static_cast<unsigned int>(D3D_SHADER_MODEL_6_0));
    EXPECT_NE(resources.shaderModelDiagnostics.find("Shader Model"), std::string::npos);
    EXPECT_TRUE(resources.dredDiagnosticsEnabled);
}

TEST(EditorLaunchArgsTests, DisabledDebugValidationDx12DeviceCreationKeepsDredDiagnosticsEnabled)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);

    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);

    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable or Shader Model 6 unsupported on this test machine";

    EXPECT_TRUE(resources.IsValid());
    EXPECT_TRUE(resources.shaderModel6Supported);
    EXPECT_GE(resources.confirmedShaderModel, static_cast<unsigned int>(D3D_SHADER_MODEL_6_0));
    EXPECT_NE(resources.shaderModelDiagnostics.find("Shader Model"), std::string::npos);
    EXPECT_TRUE(resources.dredDiagnosticsEnabled);
}
#endif
