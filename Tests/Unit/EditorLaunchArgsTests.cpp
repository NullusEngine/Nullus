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
    EXPECT_EQ(NLS::Editor::Core::ResolveEditorThreadedFrameSlotCount(2u), 3u);
    EXPECT_EQ(NLS::Editor::Core::ResolveEditorThreadedFrameSlotCount(3u), 4u);
    EXPECT_EQ(NLS::Editor::Core::ResolveEditorThreadedPublishRetirementWaitMs(), 8u);
}

TEST(EditorLaunchArgsTests, Dx12DeviceCreationChecksShaderModel6BeforeCommandQueues)
{
    const auto dx12SourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp";
    const std::string source = ReadTextFile(dx12SourcePath);

    const auto createResources = source.find("DX12DeviceResources CreateDX12DeviceResources(bool debugMode)");
    ASSERT_NE(createResources, std::string::npos);
    const auto windowsBranchEnd = source.find("#else", createResources);
    ASSERT_NE(windowsBranchEnd, std::string::npos);
    const auto body = source.substr(createResources, windowsBranchEnd - createResources);

    const auto createDevice = body.find("D3D12CreateDevice(resources.adapter.Get()");
    const auto shaderModelQuery = body.find("QueryDX12ShaderModel6Support(resources.device.Get())");
    const auto requiredSm6 = source.find("D3D_SHADER_MODEL_6_0");
    const auto graphicsQueue = body.find("CreateCommandQueue(&queueDesc");
    const auto capabilities = body.find("BuildDX12Capabilities");
    ASSERT_NE(createDevice, std::string::npos);
    ASSERT_NE(shaderModelQuery, std::string::npos);
    ASSERT_NE(requiredSm6, std::string::npos);
    ASSERT_NE(graphicsQueue, std::string::npos);
    ASSERT_NE(capabilities, std::string::npos);

    EXPECT_LT(createDevice, shaderModelQuery);
    EXPECT_LT(shaderModelQuery, graphicsQueue);
    EXPECT_LT(shaderModelQuery, capabilities);
    EXPECT_NE(body.find("BuildShaderModelFailureDiagnostic(shaderModelSupport)"), std::string::npos);
    EXPECT_NE(source.find("Shader Model 6.0"), std::string::npos);
    EXPECT_NE(source.find("candidateShaderModels"), std::string::npos);
    EXPECT_EQ(source.find("#if defined(D3D_SHADER_MODEL_6_"), std::string::npos);
    EXPECT_NE(source.find("static_cast<D3D_SHADER_MODEL>(0x68)"), std::string::npos);
    EXPECT_NE(source.find("static_cast<D3D_SHADER_MODEL>(0x61)"), std::string::npos);
}

TEST(EditorLaunchArgsTests, Dx12HardwareAdapterSelectionRequiresShaderModel6)
{
    const auto dx12SourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp";
    const std::string source = ReadTextFile(dx12SourcePath);

    const auto findAdapter = source.find("Microsoft::WRL::ComPtr<IDXGIAdapter1> FindHardwareAdapter");
    ASSERT_NE(findAdapter, std::string::npos);
    const auto nextFunction = source.find("NLS::Render::RHI::RHIDeviceCapabilities BuildDX12Capabilities", findAdapter);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto body = source.substr(findAdapter, nextFunction - findAdapter);

    const auto createDevice = body.find("D3D12CreateDevice(candidate.Get()");
    const auto shaderModelCheck = body.find("QueryDX12ShaderModel6Support(testDevice.Get())");
    const auto acceptAdapter = body.find("adapter = candidate");
    ASSERT_NE(createDevice, std::string::npos);
    ASSERT_NE(shaderModelCheck, std::string::npos);
    ASSERT_NE(acceptAdapter, std::string::npos);

    EXPECT_LT(createDevice, shaderModelCheck);
    EXPECT_LT(shaderModelCheck, acceptAdapter);
}

TEST(EditorLaunchArgsTests, Dx12AdapterSelectionPreservesShaderModelRejectionDiagnostic)
{
    const auto dx12SourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp";
    const std::string source = ReadTextFile(dx12SourcePath);

    const auto findAdapter = source.find("Microsoft::WRL::ComPtr<IDXGIAdapter1> FindHardwareAdapter");
    ASSERT_NE(findAdapter, std::string::npos);
    const auto nextFunction = source.find("NLS::Render::RHI::RHIDeviceCapabilities BuildDX12Capabilities", findAdapter);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto adapterBody = source.substr(findAdapter, nextFunction - findAdapter);
    EXPECT_NE(adapterBody.find("rejectionDiagnostics"), std::string::npos);
    EXPECT_NE(adapterBody.find("BuildShaderModelFailureDiagnostic(shaderModelSupport)"), std::string::npos);

    const auto createResources = source.find("DX12DeviceResources CreateDX12DeviceResources(bool debugMode)");
    ASSERT_NE(createResources, std::string::npos);
    const auto windowsBranchEnd = source.find("#else", createResources);
    ASSERT_NE(windowsBranchEnd, std::string::npos);
    const auto resourcesBody = source.substr(createResources, windowsBranchEnd - createResources);
    EXPECT_NE(resourcesBody.find("last rejected adapter"), std::string::npos);
    EXPECT_NE(resourcesBody.find("adapterRejectionDiagnostics"), std::string::npos);
}

TEST(EditorLaunchArgsTests, Dx12DeviceResourcesExposeConfirmedShaderModelSupport)
{
    const auto dx12HeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12Device.h";
    const std::string header = ReadTextFile(dx12HeaderPath);

    EXPECT_NE(header.find("shaderModel6Supported"), std::string::npos);
    EXPECT_NE(header.find("confirmedShaderModel"), std::string::npos);
    EXPECT_NE(header.find("shaderModelDiagnostics"), std::string::npos);
    EXPECT_NE(header.find("creationDiagnostics"), std::string::npos);
}

#if defined(_WIN32)
TEST(EditorLaunchArgsTests, DefaultDx12DeviceCreationSkipsDredDiagnostics)
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
    EXPECT_FALSE(resources.dredDiagnosticsEnabled);
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

TEST(EditorLaunchArgsTests, DisabledDebugValidationDx12DeviceCreationSkipsDredDiagnostics)
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
    EXPECT_FALSE(resources.dredDiagnosticsEnabled);
}
#endif
