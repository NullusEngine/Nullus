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

TEST(EditorLaunchArgsTests, SceneViewDragPreviewMeshLoadsAreOpportunisticBackgroundWork)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/SceneView.cpp");

    const auto functionStart = source.find("bool StartImportedAssetDragPreviewMeshLoad");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void AcquireImportedAssetDragPreviewResourceOwner", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("TrackOpportunisticBackgroundTask"), std::string::npos)
        << "Scene View drag preview mesh preloads are hover-only work; saturated queues should defer them without queue-full warnings.";
    EXPECT_EQ(body.find("TrackBackgroundTask"), std::string::npos)
        << "Using the mandatory background queue here logs queue-full warnings while dragging prefabs.";
}

TEST(EditorLaunchArgsTests, ImportedPrefabPreviewCommitKeepsPreviewRootUntilReady)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorActions.cpp");

    const auto functionStart = source.find("Engine::GameObject* NLS::Editor::Core::EditorActions::CommitGameObjectFromImportedPrefabPreview");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void NLS::Editor::Core::EditorActions::CompletePendingAssetDrop", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_EQ(body.find("(void)previewRenderableReady"), std::string::npos)
        << "Drag preview readiness must drive commit resolution options instead of being ignored.";
    EXPECT_EQ(body.find("CreateGameObjectFromAssetBlocking"), std::string::npos)
        << "Mouse release must not destroy a loaded preview root and synchronously instantiate a replacement.";
    EXPECT_EQ(body.find("cleanupPreviewRoot();\n    CancelPreviewResourceHandoff"), std::string::npos)
        << "A not-yet-ready preview should keep streaming to completion after release instead of cancelling preview work.";
    EXPECT_NE(body.find("ConnectExistingPrefabInstance"), std::string::npos)
        << "Release should connect the existing preview root as the final prefab instance.";
    EXPECT_NE(body.find("BuildImportedPrefabPreviewCommitResolutionOptions(previewRenderableReady)"), std::string::npos)
        << "Not-ready previews should remain hidden while their existing resource handoff continues to completion.";
}

TEST(EditorLaunchArgsTests, SceneViewImportedPrefabReleaseDoesNotFallbackToBlockingCreate)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/SceneView.cpp");

    const auto functionStart = source.find("void Editor::Panels::SceneView::HandleViewportAssetDragDrop()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void Editor::Panels::SceneView::PumpImportedAssetDragPreviewBeforeRender", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_EQ(body.find("CreateGameObjectFromAssetBlocking"), std::string::npos)
        << "Scene View mouse release must not synchronously reimport/instantiate when the drag preview is still pending.";
    EXPECT_NE(body.find("CreateGameObjectFromAssetNonBlocking"), std::string::npos)
        << "Not-ready release fallback should keep using the async asset drop pipeline with progress.";

    const auto commitHandoffOffset = body.find("auto previewCommitHandoff = m_importedAssetDragPreviewSession.EndForCommit()");
    const auto resourceHandoffOffset = body.find("auto previewResourceHandoff = CollectImportedAssetDragPreviewResourceHandoff()");
    const auto clearPreviewOffset = body.find("ClearImportedAssetDragPreview(false)");
    const auto commitOffset = body.find("CommitGameObjectFromImportedPrefabPreview");
    ASSERT_NE(commitHandoffOffset, std::string::npos);
    ASSERT_NE(resourceHandoffOffset, std::string::npos);
    ASSERT_NE(clearPreviewOffset, std::string::npos);
    ASSERT_NE(commitOffset, std::string::npos);
    EXPECT_LT(commitHandoffOffset, resourceHandoffOffset)
        << "Mouse release must end the preview session before collecting resource handoff for the final commit.";
    EXPECT_LT(resourceHandoffOffset, clearPreviewOffset)
        << "Mouse release must collect pending mesh/material/texture handoff before preview cleanup can cancel hover interest.";
    EXPECT_LT(clearPreviewOffset, commitOffset)
        << "Preview cleanup should preserve the collected handoff and then commit the existing preview root.";
    EXPECT_NE(body.find("std::move(previewResourceHandoff)"), std::string::npos)
        << "Mouse release commit must transfer preview resource handoff so not-ready loads continue to ready.";
}

TEST(EditorLaunchArgsTests, AssetBrowserWatcherStartupSchedulingFailureDoesNotCrashOrSuppressRetry)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/AssetBrowser.cpp");

    const auto functionStart = source.find("void Editor::Panels::AssetBrowser::StartWatchersAsync()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void Editor::Panels::AssetBrowser::StartWatchersSynchronously", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("try"), std::string::npos)
        << "Async watcher startup must handle JobSystem scheduling rejection locally.";
    EXPECT_NE(body.find("ScheduleAssetBrowserJobFuture"), std::string::npos);
    EXPECT_NE(body.find("m_watchersStartupQueued = true"), std::string::npos);
    EXPECT_NE(body.find("m_watchersStartupQueued = false"), std::string::npos)
        << "Rejected watcher scheduling must leave startup retryable on a later frame.";
    EXPECT_LT(body.find("ScheduleAssetBrowserJobFuture"), body.find("m_watchersStartupQueued = true"))
        << "Watcher startup should only be marked queued after JobSystem scheduling succeeds.";
    EXPECT_NE(body.find("catch (const std::exception&"), std::string::npos);
    EXPECT_NE(body.find("catch (...)"), std::string::npos);
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
