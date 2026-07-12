#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "Core/EditorFrameLatency.h"
#include "Core/EditorActions.h"
#include "Core/EditorJobSystemPolicy.h"
#include "Core/EditorLaunchArgs.h"
#include "Core/StartupSceneReadyGate.h"
#include "Jobs/JobSystem.h"
#include "ResourceManagement/MaterialManager.h"
#include "ResourceManagement/MeshManager.h"
#include "ResourceManagement/TextureManager.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Backends/DX12/DX12Device.h"
#include "Rendering/Settings/DriverSettings.h"

namespace
{
    class EditorLaunchTestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        EditorLaunchTestTexture()
        {
            m_desc.extent = {1u, 1u, 1u};
            m_desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
            m_desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
            m_desc.debugName = "EditorLaunchTestTexture";
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override
        {
            return NLS::Render::RHI::ResourceState::Unknown;
        }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    using StartupGateCounts = NLS::Editor::Core::StartupSceneRendererResourcePendingCounts;

    struct StartupGateHarness
    {
        std::vector<StartupGateCounts> samples;
        size_t sampleIndex = 0u;
        bool running = true;
        std::chrono::milliseconds now = std::chrono::milliseconds(0);
        uint32_t pumpCount = 0u;
        uint32_t progressCount = 0u;
        uint32_t logCount = 0u;
        uint32_t sleepCount = 0u;

        StartupGateCounts GetPendingCounts()
        {
            if (samples.empty())
                return {};
            const size_t index = sampleIndex < samples.size() ? sampleIndex : samples.size() - 1u;
            if (sampleIndex + 1u < samples.size())
                ++sampleIndex;
            return samples[index];
        }

        void PumpFrame()
        {
            ++pumpCount;
            now += std::chrono::milliseconds(100);
        }

        void PresentProgress()
        {
            ++progressCount;
        }

        void LogProgress(
            std::chrono::milliseconds,
            uint32_t,
            const StartupGateCounts&)
        {
            ++logCount;
        }

        void Sleep()
        {
            ++sleepCount;
            now += std::chrono::milliseconds(1);
        }
    };

    NLS::Editor::Core::StartupSceneRendererResourceWaitResult RunStartupGateHarness(
        StartupGateHarness& harness,
        const std::chrono::milliseconds hardTimeout = std::chrono::milliseconds(1000),
        const std::chrono::milliseconds stallTimeout = std::chrono::milliseconds(1000))
    {
        return NLS::Editor::Core::WaitForStartupSceneRendererResourcesUntilReady(
            [&harness]() { return harness.now; },
            [&harness]() { return harness.running; },
            [&harness]() { return harness.GetPendingCounts(); },
            [&harness]() { harness.PumpFrame(); },
            [&harness]() { harness.PresentProgress(); },
            [&harness](
                const std::chrono::milliseconds elapsed,
                const uint32_t frameCount,
                const StartupGateCounts& counts)
            {
                harness.LogProgress(elapsed, frameCount, counts);
            },
            [&harness]() { harness.Sleep(); },
            hardTimeout,
            stallTimeout,
            std::chrono::milliseconds(250),
            std::chrono::milliseconds(250));
    }

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

TEST(EditorLaunchArgsTests, ParsesThumbnailTelemetrySummaryOutput)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-thumbnail-telemetry-summary",
        "Logs/thumbnail-telemetry.txt",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.hasDiagnosticsOverride);
    EXPECT_EQ(
        parsed.diagnosticsSettings.editorThumbnailTelemetrySummaryOutput,
        "Logs/thumbnail-telemetry.txt");
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateNoPendingWorkDoesNotPumpFrames)
{
    StartupGateHarness harness;
    harness.samples = {{}};

    const auto result = RunStartupGateHarness(harness);

    EXPECT_EQ(result.status, NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Ready);
    EXPECT_EQ(result.frameCount, 0u);
    EXPECT_EQ(harness.pumpCount, 0u);
    EXPECT_EQ(harness.sleepCount, 0u);
    EXPECT_EQ(harness.progressCount, 0u);
}

TEST(EditorLaunchArgsTests, StartupSceneFinalizationRunsReadinessFrameAndGpuStepsInOrder)
{
    std::vector<std::string> steps;
    const auto status = NLS::Editor::Core::FinalizeStartupSceneBeforeWindow(
        [&steps]() { steps.emplace_back("initial-ready"); return true; },
        [&steps]() { steps.emplace_back("final-frame"); },
        [&steps]() { steps.emplace_back("submission-drain"); return true; },
        [&steps]() { steps.emplace_back("stabilized-ready"); return true; },
        [&steps]() { steps.emplace_back("gpu-wait"); return true; });

    EXPECT_EQ(status, NLS::Editor::Core::StartupSceneFinalizationStatus::Ready);
    EXPECT_EQ(
        steps,
        (std::vector<std::string>{
            "initial-ready",
            "final-frame",
            "submission-drain",
            "stabilized-ready",
            "gpu-wait"}));
}

TEST(EditorLaunchArgsTests, StartupSceneFinalizationStopsWhenSubmissionDrainFails)
{
    uint32_t callCount = 0u;
    const auto status = NLS::Editor::Core::FinalizeStartupSceneBeforeWindow(
        [&callCount]() { ++callCount; return true; },
        [&callCount]() { ++callCount; },
        [&callCount]() { ++callCount; return false; },
        [&callCount]() { ++callCount; return true; },
        [&callCount]() { ++callCount; return true; });

    EXPECT_EQ(status, NLS::Editor::Core::StartupSceneFinalizationStatus::SubmissionDrainFailed);
    EXPECT_EQ(callCount, 3u);
}

TEST(EditorLaunchArgsTests, StartupSceneFinalizationReportsGpuWaitFailure)
{
    const auto status = NLS::Editor::Core::FinalizeStartupSceneBeforeWindow(
        []() { return true; },
        []() {},
        []() { return true; },
        []() { return true; },
        []() { return false; });

    EXPECT_EQ(status, NLS::Editor::Core::StartupSceneFinalizationStatus::GpuWaitFailed);
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateWaitsForActiveResolutionStateFinalization)
{
    StartupGateHarness harness;
    harness.samples = {
        {0u, 0u, 1u},
        {0u, 0u, 0u}
    };

    const auto result = RunStartupGateHarness(harness);

    EXPECT_EQ(result.status, NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Ready);
    EXPECT_EQ(result.frameCount, 1u);
    EXPECT_EQ(harness.pumpCount, 1u);
}

TEST(EditorLaunchArgsTests, StartupSceneReadyHardTimeoutScalesWithEffectiveBackgroundWorkers)
{
    EXPECT_EQ(
        NLS::Editor::Core::ResolveStartupSceneRendererResourceHardTimeout(16u),
        std::chrono::seconds(75));
    EXPECT_EQ(
        NLS::Editor::Core::ResolveStartupSceneRendererResourceHardTimeout(8u),
        std::chrono::seconds(105));
    EXPECT_EQ(
        NLS::Editor::Core::ResolveStartupSceneRendererResourceHardTimeout(5u),
        std::chrono::seconds(141));
    EXPECT_EQ(
        NLS::Editor::Core::ResolveStartupSceneRendererResourceHardTimeout(2u),
        std::chrono::seconds(285));
    EXPECT_EQ(
        NLS::Editor::Core::ResolveStartupSceneRendererResourceHardTimeout(0u),
        std::chrono::seconds(300));
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateTimesOutWhenPendingCountsStopChanging)
{
    StartupGateHarness harness;
    harness.samples = {{3u, 2u}};

    const auto result = RunStartupGateHarness(
        harness,
        std::chrono::seconds(10),
        std::chrono::milliseconds(250));

    EXPECT_EQ(result.status, NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Timeout);
    EXPECT_EQ(result.timeoutReason, NLS::Editor::Core::StartupSceneRendererResourceTimeoutReason::Stalled);
    EXPECT_LT(result.elapsed, std::chrono::seconds(10));
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateProgressResetsStallTimeout)
{
    StartupGateHarness harness;
    harness.samples = {
        {3u, 1u},
        {3u, 1u},
        {2u, 1u},
        {2u, 1u},
        {1u, 1u},
        {0u, 0u}
    };

    const auto result = RunStartupGateHarness(
        harness,
        std::chrono::seconds(10),
        std::chrono::milliseconds(250));

    EXPECT_EQ(result.status, NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Ready);
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateHardLimitWinsWhileCountsKeepChanging)
{
    StartupGateHarness harness;
    harness.samples = {
        {3u, 0u},
        {2u, 1u},
        {1u, 2u},
        {2u, 1u},
        {1u, 2u}
    };

    const auto result = RunStartupGateHarness(
        harness,
        std::chrono::milliseconds(250),
        std::chrono::seconds(10));

    EXPECT_EQ(result.status, NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Timeout);
    EXPECT_EQ(result.timeoutReason, NLS::Editor::Core::StartupSceneRendererResourceTimeoutReason::HardLimit);
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateCountOscillationDoesNotResetStallTimeout)
{
    StartupGateHarness harness;
    for (size_t index = 0u; index < 20u; ++index)
        harness.samples.push_back({index % 2u == 0u ? 1u : 2u, 0u});

    const auto result = RunStartupGateHarness(
        harness,
        std::chrono::seconds(2),
        std::chrono::milliseconds(250));

    EXPECT_EQ(result.status, NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Timeout);
    EXPECT_EQ(result.timeoutReason, NLS::Editor::Core::StartupSceneRendererResourceTimeoutReason::Stalled);
    EXPECT_LT(result.elapsed, std::chrono::seconds(2))
        << "Oscillating counts are activity, but they are not monotonic readiness progress.";
}

TEST(EditorLaunchArgsTests, EditorJobWorkersShareOneHardwareBudget)
{
    const struct
    {
        uint32_t hardwareConcurrency;
        uint32_t expectedForeground;
        uint32_t expectedBackground;
    } cases[] = {
        {0u, 1u, 1u},
        {1u, 1u, 1u},
        {2u, 1u, 1u},
        {4u, 1u, 1u},
        {8u, 2u, 3u},
        {12u, 3u, 6u},
        {16u, 4u, 9u},
        {20u, 1u, 16u},
        {64u, 15u, 16u}
    };

    for (const auto& testCase : cases)
    {
        const auto budget = NLS::Editor::Core::ResolveEditorJobWorkerBudget(testCase.hardwareConcurrency);
        EXPECT_EQ(budget.foregroundWorkerCount, testCase.expectedForeground);
        EXPECT_EQ(budget.backgroundWorkerCount, testCase.expectedBackground);
        const uint32_t resolvedHardware = testCase.hardwareConcurrency == 0u ? 4u : testCase.hardwareConcurrency;
        const uint32_t expectedTotalBudget = std::min(
            31u,
            std::max(2u, resolvedHardware > 3u ? resolvedHardware - 3u : 2u));
        EXPECT_LE(budget.foregroundWorkerCount + budget.backgroundWorkerCount, expectedTotalBudget);
    }
}

TEST(EditorLaunchArgsTests, EditorJobWorkerOverrideKeepsTheSameTotalBudget)
{
    const auto twoWorkers = NLS::Editor::Core::ResolveEditorJobWorkerBudget(20u, 2u);
    EXPECT_EQ(twoWorkers.foregroundWorkerCount, 15u);
    EXPECT_EQ(twoWorkers.backgroundWorkerCount, 2u);

    const auto eightWorkers = NLS::Editor::Core::ResolveEditorJobWorkerBudget(20u, 8u);
    EXPECT_EQ(eightWorkers.foregroundWorkerCount, 9u);
    EXPECT_EQ(eightWorkers.backgroundWorkerCount, 8u);

    const auto sixteenWorkers = NLS::Editor::Core::ResolveEditorJobWorkerBudget(20u, 16u);
    EXPECT_EQ(sixteenWorkers.foregroundWorkerCount, 1u);
    EXPECT_EQ(sixteenWorkers.backgroundWorkerCount, 16u);

    const auto clampedWorkers = NLS::Editor::Core::ResolveEditorJobWorkerBudget(64u, 64u);
    EXPECT_EQ(clampedWorkers.backgroundWorkerCount, 16u);
    EXPECT_EQ(clampedWorkers.foregroundWorkerCount, 15u);
}

TEST(EditorLaunchArgsTests, JobSystemReportsEffectiveBackgroundWorkerCount)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to isolate the global JobSystem.";
#else
    NLS::Base::Jobs::ResetJobSystemForTesting();
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    config.backgroundWorkerCount = 3u;
    ASSERT_TRUE(NLS::Base::Jobs::TryInitializeJobSystem(config));
    EXPECT_EQ(NLS::Base::Jobs::GetJobWorkerCount(), 1u);
    EXPECT_EQ(NLS::Base::Jobs::GetBackgroundJobWorkerCount(), 3u);
    NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
}

TEST(EditorLaunchArgsTests, JobSystemResolvesAutomaticBackgroundWorkerCountFromHardware)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to isolate the global JobSystem.";
#else
    NLS::Base::Jobs::ResetJobSystemForTesting();
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 1u;
    config.backgroundWorkerCount = NLS::Base::Jobs::kAutoJobWorkerCount;
    ASSERT_TRUE(NLS::Base::Jobs::TryInitializeJobSystem(config));

    const uint32_t hardwareWorkers = std::thread::hardware_concurrency();
    const uint32_t expectedWorkers = hardwareWorkers <= 1u
        ? 1u
        : std::min(64u, hardwareWorkers - 1u);
    EXPECT_EQ(NLS::Base::Jobs::GetBackgroundJobWorkerCount(), expectedWorkers);
    NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
}

TEST(EditorLaunchArgsTests, RendererResourceTailReplacesStaleTextureWithDeclaredCachedTexture)
{
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});

    constexpr const char* declaredPath =
        "Library/Artifacts/12/123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    auto declaredTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        std::make_shared<EditorLaunchTestTexture>(),
        1u,
        1u);
    auto staleTexture = NLS::Render::Resources::Texture2D::WrapExternal(nullptr, 1u, 1u);
    ASSERT_NE(declaredTexture, nullptr);
    ASSERT_NE(staleTexture, nullptr);

    const auto resolvedDeclaredPath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(declaredPath);
    declaredTexture->path = resolvedDeclaredPath;
    staleTexture->path = "Library/Artifacts/ff/stale-normal-map";
    auto* expectedTexture = textureManager.RegisterResource(
        resolvedDeclaredPath,
        declaredTexture.release());
    ASSERT_NE(expectedTexture, nullptr);

    NLS::Render::Resources::Material material;
    material.SetTextureResourcePath("_NormalMap", declaredPath);
    material.SetRawParameter("_NormalMap", staleTexture.get());

    EXPECT_TRUE(NLS::Editor::Core::BindCachedRendererResourceMaterialTexture(
        material,
        "_NormalMap",
        declaredPath,
        textureManager));
    const auto* parameter = material.GetParameterBlock().TryGet("_NormalMap");
    ASSERT_NE(parameter, nullptr);
    ASSERT_EQ(parameter->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter), expectedTexture);

    textureManager.UnloadResources();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
}

TEST(EditorLaunchArgsTests, RendererResourceTailReplacesSamePathNonManagerTexture)
{
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});

    constexpr const char* declaredPath =
        "Library/Artifacts/56/56789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234";
    const auto resolvedDeclaredPath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(declaredPath);
    auto currentTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        std::make_shared<EditorLaunchTestTexture>(), 1u, 1u);
    auto oldTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        std::make_shared<EditorLaunchTestTexture>(), 1u, 1u);
    currentTexture->path = resolvedDeclaredPath;
    oldTexture->path = resolvedDeclaredPath;
    auto* expectedTexture = textureManager.RegisterResource(
        resolvedDeclaredPath,
        currentTexture.release());
    ASSERT_NE(expectedTexture, nullptr);

    NLS::Render::Resources::Material material;
    material.SetTextureResourcePath("_NormalMap", declaredPath);
    material.SetRawParameter("_NormalMap", oldTexture.get());

    EXPECT_TRUE(NLS::Editor::Core::BindCachedRendererResourceMaterialTexture(
        material, "_NormalMap", declaredPath, textureManager));
    const auto* parameter = material.GetParameterBlock().TryGet("_NormalMap");
    ASSERT_NE(parameter, nullptr);
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter), expectedTexture);

    textureManager.UnloadResources();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
}

TEST(EditorLaunchArgsTests, SceneLoadRendererResourceReadinessSnapshotIsEmptyWithoutRegisteredWork)
{
    const auto snapshot = NLS::Editor::Core::GetSceneLoadRendererResourceReadinessSnapshot();
    EXPECT_EQ(snapshot.activeStateCount, 0u);
    EXPECT_EQ(snapshot.pendingTaskCount, 0u);
    EXPECT_EQ(snapshot.pendingTextureLoadCount, 0u);
    EXPECT_FALSE(snapshot.HasPendingResources());
}

TEST(EditorLaunchArgsTests, RendererResourceTailRejectsCachedTextureWithoutGpuHandle)
{
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});

    constexpr const char* declaredPath =
        "Library/Artifacts/34/3456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef012";
    auto declaredTexture = NLS::Render::Resources::Texture2D::WrapExternal(nullptr, 1u, 1u);
    ASSERT_NE(declaredTexture, nullptr);
    const auto resolvedDeclaredPath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(declaredPath);
    declaredTexture->path = resolvedDeclaredPath;
    textureManager.RegisterResource(resolvedDeclaredPath, declaredTexture.release());

    NLS::Render::Resources::Material material;
    material.SetTextureResourcePath("_NormalMap", declaredPath);

    EXPECT_FALSE(NLS::Editor::Core::BindCachedRendererResourceMaterialTexture(
        material,
        "_NormalMap",
        declaredPath,
        textureManager));

    textureManager.UnloadResources();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGatePumpsUntilTasksAndTexturesDrain)
{
    StartupGateHarness harness;
    harness.samples = {
        {2u, 1u},
        {1u, 1u},
        {0u, 1u},
        {0u, 0u}
    };

    const auto result = RunStartupGateHarness(harness);

    EXPECT_EQ(result.status, NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Ready);
    EXPECT_EQ(result.frameCount, 3u);
    EXPECT_EQ(result.pendingCounts.taskCount, 0u);
    EXPECT_EQ(result.pendingCounts.textureLoadCount, 0u);
    EXPECT_EQ(harness.pumpCount, 3u);
    EXPECT_EQ(harness.sleepCount, 3u);
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateDoesNotFinishWhileTexturesRemainPending)
{
    StartupGateHarness harness;
    harness.samples = {
        {0u, 2u},
        {0u, 1u},
        {0u, 1u},
        {0u, 1u}
    };

    const auto result = RunStartupGateHarness(
        harness,
        std::chrono::milliseconds(250),
        std::chrono::milliseconds(250));

    EXPECT_EQ(result.status, NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Timeout);
    EXPECT_EQ(result.pendingCounts.taskCount, 0u);
    EXPECT_EQ(result.pendingCounts.textureLoadCount, 1u);
    EXPECT_GT(harness.pumpCount, 0u);
}

TEST(EditorLaunchArgsTests, StartupSceneReadyTimeoutReportsExplicitDegradedFailOpenState)
{
    NLS::Editor::Core::StartupSceneRendererResourceWaitResult result;
    result.status = NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Timeout;
    result.timeoutReason = NLS::Editor::Core::StartupSceneRendererResourceTimeoutReason::HardLimit;
    result.elapsed = std::chrono::milliseconds(45004);
    result.frameCount = 812u;
    result.pendingCounts = {3u, 17u};

    EXPECT_EQ(
        NLS::Editor::Core::FormatStartupSceneRendererResourceDegradedOpenDiagnostic(result),
        "[Startup] WaitForStartupSceneRendererResources status=degraded-open failOpen=true "
        "reason=hard-limit elapsedMs=45004 frames=812 pendingTasks=3 pendingTextureLoads=17 activeStates=0");
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateCancelsWhenWindowStopsRunning)
{
    StartupGateHarness harness;
    harness.samples = {
        {1u, 0u},
        {1u, 0u}
    };
    bool firstRunningCheck = true;
    const auto result = NLS::Editor::Core::WaitForStartupSceneRendererResourcesUntilReady(
        [&harness]() { return harness.now; },
        [&]()
        {
            if (firstRunningCheck)
            {
                firstRunningCheck = false;
                return true;
            }
            return false;
        },
        [&harness]() { return harness.GetPendingCounts(); },
        [&harness]() { harness.PumpFrame(); },
        [&harness]() { harness.PresentProgress(); },
        [&harness](
            const std::chrono::milliseconds elapsed,
            const uint32_t frameCount,
            const StartupGateCounts& counts)
        {
            harness.LogProgress(elapsed, frameCount, counts);
        },
        [&harness]() { harness.Sleep(); },
        std::chrono::milliseconds(1000),
        std::chrono::milliseconds(1000),
        std::chrono::milliseconds(250),
        std::chrono::milliseconds(250));

    EXPECT_EQ(result.status, NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Cancelled);
    EXPECT_EQ(harness.pumpCount, 1u);
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateCancelWinsWhenPendingDrainsDuringPump)
{
    StartupGateHarness harness;
    harness.samples = {
        {1u, 0u},
        {0u, 0u}
    };

    const auto result = NLS::Editor::Core::WaitForStartupSceneRendererResourcesUntilReady(
        [&harness]() { return harness.now; },
        [&harness]() { return harness.running; },
        [&harness]() { return harness.GetPendingCounts(); },
        [&harness]()
        {
            harness.PumpFrame();
            harness.running = false;
        },
        [&harness]() { harness.PresentProgress(); },
        [&harness](
            const std::chrono::milliseconds elapsed,
            const uint32_t frameCount,
            const StartupGateCounts& counts)
        {
            harness.LogProgress(elapsed, frameCount, counts);
        },
        [&harness]() { harness.Sleep(); },
        std::chrono::milliseconds(1000),
        std::chrono::milliseconds(1000),
        std::chrono::milliseconds(250),
        std::chrono::milliseconds(250));

    EXPECT_EQ(result.status, NLS::Editor::Core::StartupSceneRendererResourceWaitStatus::Cancelled);
    EXPECT_EQ(result.pendingCounts.taskCount, 0u);
    EXPECT_EQ(result.pendingCounts.textureLoadCount, 0u);
}

TEST(EditorLaunchArgsTests, ParsesValidationAssetBrowserFolder)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-validation-asset-browser-folder",
        "Assets/Model/main_sponza",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.hasDiagnosticsOverride);
    EXPECT_EQ(
        parsed.diagnosticsSettings.editorValidationAssetBrowserFolder,
        "Assets/Model/main_sponza");
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, ParsesPrefabDragProxyValidationSummaryOutput)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-validation-prefab-drag-proxy-summary",
        "Logs/prefab-drag-proxy.txt",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.hasDiagnosticsOverride);
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, EditorWritesPrefabDragProxyValidationSummaryWhenConfigured)
{
    const auto diagnosticsHeader = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Settings/EngineDiagnosticsSettings.h");
    EXPECT_NE(diagnosticsHeader.find("editorValidationPrefabDragProxySummaryOutput"), std::string::npos)
        << "The prefab drag proxy validation output path must live in diagnostics settings so it is off by default.";

    const auto launchArgs = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/EditorLaunchArgs.cpp");
    EXPECT_NE(
        launchArgs.find("--editor-validation-prefab-drag-proxy-summary"),
        std::string::npos);
    EXPECT_NE(
        launchArgs.find("editorValidationPrefabDragProxySummaryOutput = argv[++i]"),
        std::string::npos);

    const auto editorSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Editor.cpp");
    EXPECT_NE(editorSource.find("WritePrefabDragProxyValidationSummaryIfRequested"), std::string::npos)
        << "Startup validation needs a real editor-path report for the Scene View-facing prefab drag proxy.";
    EXPECT_NE(editorSource.find("BuildSceneViewPrefabDragProxyDescriptor"), std::string::npos);
    EXPECT_NE(editorSource.find("SubmitPrefabDragProxyDebugPrimitives"), std::string::npos);
    EXPECT_NE(editorSource.find("proxyVisibleBeforeRoot=true"), std::string::npos);
    EXPECT_NE(editorSource.find("followedPlacement=true"), std::string::npos);
    EXPECT_NE(editorSource.find("visibleLineCount="), std::string::npos);
    EXPECT_NE(editorSource.find("sceneRootCreatedByProxy=false"), std::string::npos);
    EXPECT_NE(editorSource.find("sceneViewDragLoopExercised=true"), std::string::npos)
        << "The validation summary must prove the Scene View drag/drop target path itself drives the proxy.";
    EXPECT_NE(editorSource.find("sceneViewDragLoopPayloadAcceptedBeforeDelivery=true"), std::string::npos)
        << "Unity-level drag feel depends on AcceptBeforeDelivery updates before mouse release.";
    EXPECT_NE(editorSource.find("sceneViewDragLoopProxyDescriptorCount="), std::string::npos)
        << "The summary must report how many proxy descriptors came from SceneView active-drag state.";

    const auto sceneViewHeader = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/SceneView.h");
    EXPECT_NE(sceneViewHeader.find("ValidatePrefabDragProxySceneViewLoopForTesting"), std::string::npos)
        << "Runtime validation should exercise SceneView's private drag/drop handler through a test hook.";

    const auto dragDropHeader = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/Plugins/DragDrop.h");
    EXPECT_NE(dragDropHeader.find("SetDragDropTargetPayloadForTesting"), std::string::npos)
        << "SceneView validation needs a scoped ImGui drag/drop target override, not a helper-only path.";

    const auto contextHeader = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Context.h");
    EXPECT_NE(
        contextHeader.find(
            "settings.editorValidationPrefabDragProxySummaryOutput = m_diagnosticsOverride->editorValidationPrefabDragProxySummaryOutput"),
        std::string::npos)
        << "The CLI diagnostics override must be copied into Context before ApplyStartupValidationDirectives runs.";
}

TEST(EditorLaunchArgsTests, EditorWritesThumbnailTelemetrySummaryOnShutdownWhenConfigured)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Editor.cpp");

    EXPECT_NE(source.find("WriteThumbnailTelemetrySummaryIfRequested"), std::string::npos)
        << "Asset Browser thumbnail telemetry needs an editor-visible summary export path so "
           "performance work can use real capture data instead of guessing.";
}

TEST(EditorLaunchArgsTests, ValidationTraceFinishRefreshesThumbnailTelemetrySummaryBeforeClose)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Editor.cpp");
    const auto traceFinish = source.find("Editor validation TimelineProfiler trace finished");
    ASSERT_NE(traceFinish, std::string::npos);
    const auto writeSummary = source.find("WriteThumbnailTelemetrySummaryIfRequested(m_context, false)", traceFinish);
    const auto requestClose = source.find("SetShouldClose(true)", traceFinish);

    EXPECT_NE(writeSummary, std::string::npos)
        << "Automated thumbnail telemetry captures must flush after validation frames finish; "
           "otherwise a close confirmation can leave only the startup-time empty summary.";
    EXPECT_NE(requestClose, std::string::npos);
    if (writeSummary != std::string::npos && requestClose != std::string::npos)
        EXPECT_LT(writeSummary, requestClose);
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateRunsBeforeCompletingStartupProgress)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Application.cpp");

    const auto gateDefinition = source.find("WaitForStartupSceneRendererResources");
    ASSERT_NE(gateDefinition, std::string::npos)
        << "Startup must keep the progress surface visible while the restored scene's renderer resources finish.";

    const auto gateCall = source.find("if (!WaitForStartupSceneRendererResources())");
    ASSERT_NE(gateCall, std::string::npos)
        << "Startup must explicitly wait for scene renderer resources before opening the main editor.";
    EXPECT_NE(source.find("return;", gateCall), std::string::npos)
        << "Closing the editor while the startup scene-ready gate is active must not continue into opening the main UI.";

    const auto completeStartup = source.find("CompleteStartupProgress();");
    ASSERT_NE(completeStartup, std::string::npos);
    EXPECT_LT(gateCall, completeStartup)
        << "The main editor must not become visible before startup scene renderer resources are ready.";

    const auto finalReadyFrame = source.find("RunEditorFrame(0.0f);", gateCall);
    ASSERT_NE(finalReadyFrame, std::string::npos);
    const auto drainThreadedRendering = source.find("TryDrainThreadedRendering", finalReadyFrame);
    ASSERT_NE(drainThreadedRendering, std::string::npos);
    EXPECT_LT(finalReadyFrame, drainThreadedRendering);
    EXPECT_LT(drainThreadedRendering, completeStartup)
        << "The hidden resource-complete frame must finish threaded rendering before the window is shown.";
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateRechecksWorkCreatedByFinalHiddenFrame)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Application.cpp");
    const auto firstGate = source.find("if (!WaitForStartupSceneRendererResources())");
    ASSERT_NE(firstGate, std::string::npos);
    const auto finalReadyFrame = source.find("RunEditorFrame(0.0f);", firstGate);
    ASSERT_NE(finalReadyFrame, std::string::npos);
    const auto secondGate = source.find("if (!WaitForStartupSceneRendererResources())", finalReadyFrame);
    ASSERT_NE(secondGate, std::string::npos)
        << "The final hidden frame may enqueue newly visible texture work and must be stabilized before show.";
    const auto finalDrain = source.find("TryWaitForSubmittedGpuWork", secondGate);
    const auto completeStartup = source.find("CompleteStartupProgress();", secondGate);
    ASSERT_NE(finalDrain, std::string::npos);
    ASSERT_NE(completeStartup, std::string::npos);
    EXPECT_LT(finalDrain, completeStartup);
}

TEST(EditorLaunchArgsTests, EditorLogsLaunchToWindowShownLatency)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Main.cpp");

    EXPECT_NE(source.find("const auto launchBegin = std::chrono::steady_clock::now();"), std::string::npos);
    EXPECT_NE(source.find("[Startup] EditorWindowShown elapsedMs="), std::string::npos)
        << "Benchmarks need a direct launch-to-window metric instead of inferring open speed from gate duration.";
}

TEST(EditorLaunchArgsTests, EditorWindowShownMetricRequiresConfirmedWindowVisibility)
{
    const auto mainSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Main.cpp");
    const auto applicationHeader = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Application.h");

    EXPECT_NE(applicationHeader.find("DidShowEditorWindow() const"), std::string::npos);
    const auto applicationSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Application.cpp");
    EXPECT_NE(applicationSource.find("window->IsVisible()"), std::string::npos);
    const auto visibilityCheck = mainSource.find("if (app->DidShowEditorWindow())");
    const auto metric = mainSource.find("[Startup] EditorWindowShown elapsedMs=");
    ASSERT_NE(visibilityCheck, std::string::npos);
    ASSERT_NE(metric, std::string::npos);
    EXPECT_LT(visibilityCheck, metric)
        << "A cancelled or failed hidden startup must not emit a false window-shown metric.";
}

TEST(EditorLaunchArgsTests, EditorStartupFailureReturnsNonzeroProcessExit)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Main.cpp");

    EXPECT_NE(source.find("static bool TryRun("), std::string::npos);
    EXPECT_NE(source.find("? EXIT_SUCCESS : EXIT_FAILURE"), std::string::npos)
        << "Startup exceptions must reach the process exit code so the benchmark can classify them.";
}

TEST(EditorLaunchArgsTests, FinalStartupSceneDrainFailureCannotEnterHiddenRunLoop)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Application.cpp");
    const auto failureLog = source.find("FinalStartupSceneReadyFrame drain failed");
    ASSERT_NE(failureLog, std::string::npos);
    const auto failureThrow = source.rfind("throw std::runtime_error", failureLog);
    const auto completeStartup = source.find("CompleteStartupProgress();", failureLog);
    ASSERT_NE(failureThrow, std::string::npos);
    ASSERT_NE(completeStartup, std::string::npos);
    EXPECT_LT(failureThrow, failureLog);
    EXPECT_LT(failureThrow, completeStartup);
}

TEST(EditorLaunchArgsTests, StartupSceneReadyLogExplicitlyReportsNoActiveResolutionStates)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Application.cpp");
    const auto readyLog = source.find("WaitForStartupSceneRendererResources end elapsedMs=");
    ASSERT_NE(readyLog, std::string::npos);
    const auto nextReturn = source.find("return true;", readyLog);
    ASSERT_NE(nextReturn, std::string::npos);

    const auto activeStates = source.find("activeStates=0", readyLog);
    EXPECT_NE(activeStates, std::string::npos);
    if (activeStates != std::string::npos)
        EXPECT_LT(activeStates, nextReturn);
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateYieldsToRendererResourceWorkers)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Application.cpp");

    const auto gateStart = source.find("bool Editor::Core::Application::WaitForStartupSceneRendererResources()");
    ASSERT_NE(gateStart, std::string::npos);
    const auto gateEnd = source.find("void Editor::Core::Application::Run()", gateStart);
    ASSERT_NE(gateEnd, std::string::npos);
    const auto gateBody = source.substr(gateStart, gateEnd - gateStart);

    EXPECT_NE(gateBody.find("std::this_thread::sleep_for(std::chrono::milliseconds(1))"), std::string::npos)
        << "The hidden startup gate must not busy-spin editor frames; renderer resource workers need a tiny yield to finish quickly.";
    EXPECT_NE(gateBody.find("m_context.device->PollEvents()"), std::string::npos)
        << "The startup scene-ready gate must keep processing close events while the progress surface is active.";
    EXPECT_NE(gateBody.find("RunEditorFrame(0.0f)"), std::string::npos)
        << "The gate should use the lightweight editor-frame pump that was measured to drain renderer resources quickly.";
    EXPECT_EQ(gateBody.find("TickFrame(0.0f, true)"), std::string::npos)
        << "A full TickFrame inside the gate re-runs normal frame follow-up work and regressed TestProject scene readiness.";
}

TEST(EditorLaunchArgsTests, StartupSceneReadyGateThrottlesNativeProgressUpdates)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Application.cpp");

    EXPECT_NE(source.find("kStartupSceneRendererResourceProgressDialogInterval"), std::string::npos)
        << "The startup scene-ready gate should throttle native progress dialog updates instead of updating it every frame.";

    const auto gateStart = source.find("bool Editor::Core::Application::WaitForStartupSceneRendererResources()");
    ASSERT_NE(gateStart, std::string::npos);
    const auto gateEnd = source.find("void Editor::Core::Application::Run()", gateStart);
    ASSERT_NE(gateEnd, std::string::npos);
    const auto gateBody = source.substr(gateStart, gateEnd - gateStart);

    const auto progressUpdate = gateBody.find("PresentStartupProgressFrame(\"Preparing startup scene resources\"");
    ASSERT_NE(progressUpdate, std::string::npos);
    const auto throttleCheck = gateBody.find("kStartupSceneRendererResourceProgressDialogInterval", progressUpdate);
    ASSERT_NE(throttleCheck, std::string::npos)
        << "Native progress dialog updates must be driven through the helper's dialog interval throttle.";
}

TEST(EditorLaunchArgsTests, ThumbnailTelemetrySummaryIncludesCacheEvaluationStages)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Editor.cpp");
    const auto whitelistStart = source.find("bool IsThumbnailLatencyStage");
    ASSERT_NE(whitelistStart, std::string::npos);
    const auto whitelistEnd = source.find("std::string FormatTelemetryDurationMs", whitelistStart);
    ASSERT_NE(whitelistEnd, std::string::npos);
    const auto whitelist = source.substr(whitelistStart, whitelistEnd - whitelistStart);

    EXPECT_NE(whitelist.find("ArtifactLoadTelemetryStageName(stage)"), std::string::npos);
    EXPECT_NE(whitelist.find(".starts_with(\"Thumbnail\")"), std::string::npos);
}

TEST(EditorLaunchArgsTests, DeferredSceneRendererThreadedRefreshGuardsMissingDescriptor)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    const auto functionStart = source.find("void DeferredSceneRenderer::DrawFrame()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void DeferredSceneRenderer::LoadPipelineResources", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    const auto refreshStart = body.find("DeferredSceneRenderer::DrawFrame::ThreadedSnapshotRefresh");
    ASSERT_NE(refreshStart, std::string::npos);
    const auto descriptorGet = body.find("GetDescriptor<DeferredSceneDescriptor>()", refreshStart);
    ASSERT_NE(descriptorGet, std::string::npos);
    const auto descriptorGuard = body.rfind("HasDescriptor<DeferredSceneDescriptor>()", descriptorGet);
    ASSERT_NE(descriptorGuard, std::string::npos)
        << "Threaded refresh can run after registered passes when the deferred descriptor was not attached; it must not assert while prefab prewarm/background work is active.";
}

TEST(EditorLaunchArgsTests, SceneViewPreloadsDeferredPipelineResourcesBeforeFirstVisibleSceneFrame)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp");

    const auto functionStart = source.find("MakeEditorDeferredRendererConstructionOptions");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("DebugSceneRenderer::DebugSceneRenderer", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_EQ(body.find("deferPipelineResourcesUntilFirstFrame = true"), std::string::npos)
        << "Scene View creates its renderer while the startup progress window is visible; deferring the deferred "
           "pipeline resources moves shader/material/fullscreen-quad loading onto the first resource-revealed "
           "scene frame and leaves users staring at a blank viewport longer.";
}

TEST(EditorLaunchArgsTests, DeferredSceneRendererDoesNotCaptureLightingForEmptySceneFrames)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    const auto functionStart = source.find("void DeferredSceneRenderer::BeginFrame");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void DeferredSceneRenderer::DrawFrame", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    const auto shouldCapture = body.find("shouldCaptureDeferredScene");
    ASSERT_NE(shouldCapture, std::string::npos);
    const auto captureLighting = body.find("DeferredSceneRenderer::BeginFrame::CaptureLighting", shouldCapture);
    ASSERT_NE(captureLighting, std::string::npos);
    const auto captureGuard = body.rfind("if (!shouldCaptureDeferredScene)", captureLighting);
    ASSERT_NE(captureGuard, std::string::npos)
        << "Startup scene-load placeholder frames may have helper/grid passes but zero deferred scene drawables. "
           "They must not enqueue the fullscreen lighting draw, otherwise the empty lighting pass can replace the "
           "placeholder output with black before any prefab resources are revealed.";
    const auto skipCaptureLog = body.find("SkipDeferredSceneCapture", captureGuard);
    ASSERT_NE(skipCaptureLog, std::string::npos);
    const auto skipPublish = body.rfind("m_skipThreadedFramePublish = true", skipCaptureLog);
    ASSERT_NE(skipPublish, std::string::npos)
        << "An empty scene-load placeholder frame must not publish an empty threaded deferred package over the "
           "previous viewport output; the first publish should happen when a real scene drawable is revealed.";
}

TEST(EditorLaunchArgsTests, DeferredSceneRendererDrawPathDiagnosticsAreFrameBounded)
{
    const auto header = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.h");
    EXPECT_NE(header.find("m_framePreparedDrawDiagnosticLogCount"), std::string::npos);

    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");
    const auto functionStart = source.find("void DeferredSceneRenderer::LogPreparedDrawResult");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("NLS::Render::Context::PreparedRenderSceneBuilder", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("m_framePreparedDrawDiagnosticLogCount >= 8u"), std::string::npos)
        << "Large prefabs can submit hundreds of drawables per frame; draw-path diagnostics must not flood the Console.";
    EXPECT_NE(body.find("++m_framePreparedDrawDiagnosticLogCount"), std::string::npos);
    EXPECT_NE(source.find("m_framePreparedDrawDiagnosticLogCount = 0u"), std::string::npos);
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

TEST(EditorLaunchArgsTests, SceneViewImportedPrefabDragDropDoesNotCreatePreviewState)
{
    const auto header = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/SceneView.h");
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/SceneView.cpp");

    const auto functionStart = source.find("void Editor::Panels::SceneView::HandleViewportAssetDragDrop()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void Editor::Panels::SceneView::EnsureRenderer", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_EQ(body.find("CreateGameObjectFromAssetBlocking"), std::string::npos)
        << "Scene View drag/drop must not synchronously import or instantiate while the UI is dragging.";
    EXPECT_EQ(body.find("ImportedAssetDragPreview"), std::string::npos)
        << "Scene View should not recreate imported prefab preview state while dragging.";
    EXPECT_NE(header.find("m_activeDraggedPrefabRoot"), std::string::npos)
        << "Scene View should track the formal transient prefab instance created while dragging.";
    EXPECT_NE(source.find("UpdateActivePrefabDragInstance"), std::string::npos)
        << "Scene View should move the formal dragged prefab instance with the cursor once the hot cache is ready.";
    EXPECT_NE(source.find("CancelActivePrefabDragInstance"), std::string::npos)
        << "Scene View should destroy the transient formal instance if the drag is cancelled.";
    EXPECT_NE(source.find("CommitActivePrefabDragInstance"), std::string::npos)
        << "Scene View should convert the transient formal instance into the committed scene object on release.";
}

TEST(EditorLaunchArgsTests, AssetBrowserVisiblePrefabPrewarmUsesOpportunisticBackgroundWork)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/AssetBrowser.cpp");

    const auto functionStart = source.find("void Editor::Panels::AssetBrowser::SchedulePrefabHotCachePreloadForDragPayload");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("bool Editor::Panels::AssetBrowser::LoadCachedThumbnailTexture", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("TrackOpportunisticBackgroundTask"), std::string::npos)
        << "Visible prefab prewarm is best-effort UI work and must not saturate mandatory background queues.";
    EXPECT_NE(body.find("PreloadImportedAssetHandlePrefabHotCache"), std::string::npos)
        << "Asset Browser should prewarm the formal imported-asset hot cache, not drag-preview artifacts.";
    EXPECT_EQ(body.find("SchedulePreviewPrefabHotCachePreload"), std::string::npos);
}

TEST(EditorLaunchArgsTests, AssetBrowserVisiblePrefabPrewarmRetriesForUnchangedVisibleItems)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/AssetBrowser.cpp");

    const auto functionStart = source.find("void Editor::Panels::AssetBrowser::SetVisibleThumbnailItems");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void Editor::Panels::AssetBrowser::DrawProjectAssetBrowser", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    const auto unchangedBranch = body.find("nextFingerprint == m_visibleThumbnailFingerprint");
    ASSERT_NE(unchangedBranch, std::string::npos);
    const auto returnOffset = body.find("return;", unchangedBranch);
    ASSERT_NE(returnOffset, std::string::npos);
    EXPECT_NE(
        body.substr(unchangedBranch, returnOffset - unchangedBranch).find(
            "FlushPendingVisiblePrefabHotCachePreload()"),
        std::string::npos)
        << "Visible prefab prewarm must retry while the same asset cells remain visible; a failed or not-yet-ready first attempt cannot be permanent.";

    const auto flushStart = source.find("void Editor::Panels::AssetBrowser::FlushPendingVisiblePrefabHotCachePreload");
    ASSERT_NE(flushStart, std::string::npos);
    const auto flushEnd = source.find("bool Editor::Panels::AssetBrowser::LoadCachedThumbnailTexture", flushStart);
    ASSERT_NE(flushEnd, std::string::npos);
    const auto flushBody = source.substr(flushStart, flushEnd - flushStart);
    EXPECT_NE(flushBody.find("SchedulePrefabHotCachePreloadForVisibleItems(m_visibleThumbnailItems)"), std::string::npos)
        << "Pending visible prefab prewarm should still schedule the current visible cells once the Asset Browser is idle.";
}

TEST(EditorLaunchArgsTests, PrefabHotCachePreloadLoadsWhenPreparedCacheIsCold)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/EditorAssetDragDropBridge.cpp");

    const auto functionStart = source.find("bool EditorAssetDragDropBridge::PreloadPreparedPrefabHotCache");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("UnifiedPrefabLoadResult EditorAssetDragDropBridge::LoadUnifiedPrefab", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("TryLoadPersistentPreparedPrefabCache"), std::string::npos);
    EXPECT_NE(body.find("LoadUnifiedPrefabShared(request)"), std::string::npos)
        << "Visible prefab prewarm must populate the hot cache even when no prepared cache file exists yet.";
    EXPECT_NE(body.find("loaded.prefab != nullptr"), std::string::npos);
}

TEST(EditorLaunchArgsTests, AssetBrowserHoveredPrefabPrewarmsBeforeDragStarts)
{
    const auto header = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/AssetBrowser.h");
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Panels/AssetBrowser.cpp");

    EXPECT_NE(header.find("SchedulePrefabHotCachePreloadForHoveredItem"), std::string::npos)
        << "Unity-feel prefab drags need a hover/dwell prewarm hook before the user starts dragging.";

    const auto functionStart = source.find("void Editor::Panels::AssetBrowser::DrawCurrentFolderGrid()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void Editor::Panels::AssetBrowser::HandleProjectAssetBrowserDroppedFiles", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    const auto hoveredOffset = body.find("const bool hovered = ImGui::IsItemHovered()");
    ASSERT_NE(hoveredOffset, std::string::npos);
    const auto hoverPrewarmOffset = body.find("SchedulePrefabHotCachePreloadForHoveredItem(item, hovered)", hoveredOffset);
    ASSERT_NE(hoverPrewarmOffset, std::string::npos)
        << "A hovered model/prefab cell should begin hot-cache prewarm before drag source creation.";
    const auto dragSourceOffset = body.find("DrawProjectGridItemDragSource(item)", hoveredOffset);
    ASSERT_NE(dragSourceOffset, std::string::npos);
    EXPECT_LT(hoverPrewarmOffset, dragSourceOffset)
        << "Hover prewarm must get a frame or more of lead time before drag payload publication.";

    const auto helperStart = source.find("void Editor::Panels::AssetBrowser::SchedulePrefabHotCachePreloadForHoveredItem(");
    ASSERT_NE(helperStart, std::string::npos);
    const auto helperEnd = source.find("void Editor::Panels::AssetBrowser::SchedulePrefabHotCachePreloadForVisibleItems", helperStart);
    ASSERT_NE(helperEnd, std::string::npos);
    const auto helperBody = source.substr(helperStart, helperEnd - helperStart);
    EXPECT_NE(helperBody.find("nullptr"), std::string::npos)
        << "Hover prewarm must not run asset database freshness checks on the mouse-hover frame.";
    EXPECT_EQ(helperBody.find("m_projectAssetDatabaseSnapshot"), std::string::npos)
        << "Freshness validation belongs to drag/final-drop paths, not opportunistic hover lead time.";
    EXPECT_NE(header.find("struct HoveredPrefabHotCachePreloadIdentity"), std::string::npos)
        << "The hover debounce identity should be grouped so future identity fields are not omitted accidentally.";
    EXPECT_NE(header.find("m_lastHoveredPrefabHotCachePreloadIdentity"), std::string::npos)
        << "Hovering the same prefab/model cell across frames should not repeatedly enter the preload gate.";
    const auto repeatGuardOffset = helperBody.find("m_lastHoveredPrefabHotCachePreloadIdentity");
    ASSERT_NE(repeatGuardOffset, std::string::npos);
    const auto payloadBuildOffset = helperBody.find("MakeAssetBrowserItemDragPayload");
    ASSERT_NE(payloadBuildOffset, std::string::npos);
    EXPECT_LT(repeatGuardOffset, payloadBuildOffset)
        << "The same-hover fast return should happen before payload construction.";
    EXPECT_EQ(helperBody.find("hoverPreloadKey"), std::string::npos)
        << "The same-hover guard should compare cached item fields directly instead of building a string key each frame.";
    EXPECT_EQ(helperBody.find("std::to_string"), std::string::npos)
        << "Hover prewarm is a UI hot path; it should not stringify enum fields before the fast return.";
}

TEST(EditorLaunchArgsTests, SceneViewHoverDragUsesHotCacheOnlyPrefabDrop)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto updateStart = source.find("void Editor::Panels::SceneView::UpdateActivePrefabDragInstance");
    ASSERT_NE(updateStart, std::string::npos);
    const auto updateEnd = source.find("void Editor::Panels::SceneView::CancelActivePrefabDragInstance", updateStart);
    ASSERT_NE(updateEnd, std::string::npos);
    const auto body = source.substr(updateStart, updateEnd - updateStart);

    EXPECT_NE(body.find("TryDropImportedAssetHandleFromHotCacheIntoHierarchy"), std::string::npos)
        << "Hover-drag frames may only instantiate already-prewarmed formal prefabs.";
    EXPECT_EQ(body.find("DropImportedAssetHandleIntoHierarchy("), std::string::npos)
        << "The normal drop path can synchronously read/import prefab artifacts and must only run on release.";
}

TEST(EditorLaunchArgsTests, SceneViewHoverDragCancelReleasesPrefabResourceOwners)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto cancelStart = source.find("void Editor::Panels::SceneView::CancelActivePrefabDragInstance");
    ASSERT_NE(cancelStart, std::string::npos);
    const auto cancelEnd = source.find("bool Editor::Panels::SceneView::CommitActivePrefabDragInstance", cancelStart);
    ASSERT_NE(cancelEnd, std::string::npos);
    const auto body = source.substr(cancelStart, cancelEnd - cancelStart);

    const auto cleanupOffset = body.find("CleanupPrefabInstanceMarkedDestroy");
    const auto destroyOffset = body.find("scene->DestroyGameObject(*root)");
    const auto collectOffset = body.find("scene->CollectGarbages()");

    ASSERT_NE(cleanupOffset, std::string::npos)
        << "Cancelling a formal transient prefab drag must release scene-owned renderer resources as well as removing the prefab instance.";
    EXPECT_EQ(body.find("RemoveRootInstance(*root)"), std::string::npos)
        << "Scene View drag cancellation should use the shared prefab cleanup path instead of registry-only removal.";
    ASSERT_NE(destroyOffset, std::string::npos);
    ASSERT_NE(collectOffset, std::string::npos);
    EXPECT_LT(cleanupOffset, destroyOffset)
        << "Prefab resource owners must be released while the root is still registered as a prefab instance.";
    EXPECT_LT(cleanupOffset, collectOffset)
        << "Prefab cleanup should run before the scene collects the transient drag root.";
}

TEST(EditorLaunchArgsTests, SceneViewHoverDragUsesSceneViewDropTargetForPerFrameUpdates)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto handleStart = source.find("void Editor::Panels::SceneView::HandleViewportAssetDragDrop()");
    ASSERT_NE(handleStart, std::string::npos);
    const auto handleEnd = source.find("void Editor::Panels::SceneView::UpdateActivePrefabDragInstance", handleStart);
    ASSERT_NE(handleEnd, std::string::npos);
    const auto body = source.substr(handleStart, handleEnd - handleStart);

    EXPECT_NE(body.find("m_activeDraggedPrefabPayload.has_value()"), std::string::npos)
        << "Scene View must keep the live payload cached once prefab dragging starts.";
    EXPECT_EQ(body.find("PeekDragDropPayload"), std::string::npos)
        << "Scene View must not globally peek editor asset drags; otherwise it can preview or commit assets dragged over another panel.";
    EXPECT_NE(body.find("BeginDragDropTarget()"), std::string::npos);
    EXPECT_NE(body.find("AcceptDragDropPayload("), std::string::npos);
    EXPECT_NE(body.find("NLS::Editor::Assets::kEditorAssetDragPayloadType"), std::string::npos)
        << "Editor asset prefab preview updates should only run while the Scene View image is the active drop target.";
    EXPECT_NE(body.find("UI::DragDropTargetFlags::AcceptBeforeDelivery"), std::string::npos)
        << "Scene View needs before-delivery updates to move the transient prefab while the cursor is over the viewport.";
    EXPECT_NE(body.find("CommitActivePrefabDragInstance()"), std::string::npos)
        << "Mouse release should still drive the live prefab instance commit path.";
    EXPECT_NE(body.find("m_activeDraggedPrefabCommitPending = true"), std::string::npos)
        << "If delivery happens before the hot-cache root is ready, the Scene View should keep the drop alive until background prewarm finishes.";
    const auto deliveredOffset = body.find("payloadView.delivered");
    const auto commitPendingOffset = body.find("m_activeDraggedPrefabCommitPending = true", deliveredOffset);
    const auto coldReleaseBranchOffset = body.rfind("else", commitPendingOffset);
    ASSERT_NE(deliveredOffset, std::string::npos);
    ASSERT_NE(commitPendingOffset, std::string::npos)
        << "If delivery happens before the hot-cache root is ready, the Scene View should keep the drop alive until background prewarm finishes.";
    ASSERT_NE(coldReleaseBranchOffset, std::string::npos);
    EXPECT_EQ(
        body.substr(coldReleaseBranchOffset, commitPendingOffset - coldReleaseBranchOffset).find("CancelActivePrefabDragInstance()"),
        std::string::npos)
        << "Cold prefab delivery must not cancel just because the hot-cache instance is not ready yet.";
}

TEST(EditorLaunchArgsTests, SceneViewColdPrefabReleaseSnapshotsDropSceneAndPlacement)
{
    const auto header = ReadTextFile("Project/Editor/Panels/SceneView.h");
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto handleStart = source.find("void Editor::Panels::SceneView::HandleViewportAssetDragDrop()");
    ASSERT_NE(handleStart, std::string::npos);
    const auto handleEnd = source.find("void Editor::Panels::SceneView::UpdateActivePrefabDragInstance", handleStart);
    ASSERT_NE(handleEnd, std::string::npos);
    const auto body = source.substr(handleStart, handleEnd - handleStart);

    const auto deliveredOffset = body.find("payloadView.delivered");
    ASSERT_NE(deliveredOffset, std::string::npos);
    const auto pendingOffset = body.find("m_activeDraggedPrefabCommitPending = true", deliveredOffset);
    ASSERT_NE(pendingOffset, std::string::npos);
    const auto coldReleaseBody = body.substr(deliveredOffset, pendingOffset - deliveredOffset);

    EXPECT_EQ(header.find("m_activeDraggedPrefabDropScene = nullptr"), std::string::npos)
        << "Cold releases must not retain a raw Scene* across async preload; scene unload can delete it before the hot cache becomes ready.";
    EXPECT_NE(header.find("m_activeDraggedPrefabDropSceneToken"), std::string::npos)
        << "A cold release should remember the scene generation token instead of a raw Scene*.";
    EXPECT_NE(coldReleaseBody.find("m_activeDraggedPrefabDropSceneToken = CaptureActivePrefabDragSceneToken()"), std::string::npos)
        << "A cold release must snapshot the active scene token so later scene/stage changes cancel the pending drop safely.";
    EXPECT_NE(coldReleaseBody.find("m_activeDraggedPrefabDropPlacement = ResolveActivePrefabDragPlacement"), std::string::npos)
        << "A cold release must commit at the release cursor location instead of wherever the mouse happens to be when loading finishes.";
}

TEST(EditorLaunchArgsTests, SceneViewPendingPrefabCommitUsesSnapshottedPlacementAndSceneToken)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");

    const auto updateStart = source.find("void Editor::Panels::SceneView::UpdateActivePrefabDragInstance");
    ASSERT_NE(updateStart, std::string::npos);
    const auto updateEnd = source.find("void Editor::Panels::SceneView::ClearActivePrefabDragState", updateStart);
    ASSERT_NE(updateEnd, std::string::npos);
    const auto updateBody = source.substr(updateStart, updateEnd - updateStart);
    EXPECT_NE(updateBody.find("m_activeDraggedPrefabDropSceneToken.has_value()"), std::string::npos)
        << "Pending cold-drop instantiation should validate the release-time scene token before touching the scene.";
    EXPECT_NE(updateBody.find("IsActivePrefabDragSceneTokenCurrent(*m_activeDraggedPrefabDropSceneToken)"), std::string::npos)
        << "Scene/stage changes before hot-cache readiness must cancel the pending drop instead of dereferencing stale scene state.";
    EXPECT_NE(updateBody.find("m_activeDraggedPrefabDropPlacement.has_value()"), std::string::npos)
        << "Pending cold-drop root movement should use the release-time placement snapshot.";

    const auto commitStart = source.find("bool Editor::Panels::SceneView::CommitActivePrefabDragInstance()");
    ASSERT_NE(commitStart, std::string::npos);
    const auto commitEnd = source.find("std::optional<Maths::Vector3> Editor::Panels::SceneView::ResolveActivePrefabDragPlacement", commitStart);
    ASSERT_NE(commitEnd, std::string::npos);
    const auto commitBody = source.substr(commitStart, commitEnd - commitStart);
    EXPECT_NE(commitBody.find("m_activeDraggedPrefabDropPlacement.has_value()"), std::string::npos)
        << "Commit should preserve the mouse-release placement when background prewarm finishes after delivery.";
    EXPECT_NE(commitBody.find("m_activeDraggedPrefabRootSceneToken.has_value()"), std::string::npos)
        << "Commit must verify the transient root's scene token before dereferencing the root.";
    EXPECT_LT(commitBody.find("m_activeDraggedPrefabRootSceneToken.has_value()"), commitBody.find("root->IsAlive()"))
        << "Commit must check the scene generation before dereferencing a root that may have belonged to an unloaded scene.";
    EXPECT_NE(commitBody.find("EDITOR_CONTEXT(activePrefabStage)->dirty = true"), std::string::npos)
        << "Committing into an active Prefab Stage should dirty the stage, not blindly dirty the main current scene.";
}

TEST(EditorLaunchArgsTests, SceneViewCancelUsesRootSceneTokenBeforeTouchingTransientRoot)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto cancelStart = source.find("void Editor::Panels::SceneView::CancelActivePrefabDragInstance");
    ASSERT_NE(cancelStart, std::string::npos);
    const auto cancelEnd = source.find("bool Editor::Panels::SceneView::CommitActivePrefabDragInstance", cancelStart);
    ASSERT_NE(cancelEnd, std::string::npos);
    const auto body = source.substr(cancelStart, cancelEnd - cancelStart);

    const auto tokenCheckOffset = body.find("!IsActivePrefabDragSceneTokenCurrent(*rootSceneToken)");
    const auto cleanupOffset = body.find("CleanupPrefabInstanceMarkedDestroy");
    const auto destroyOffset = body.find("DestroyGameObject");
    ASSERT_NE(tokenCheckOffset, std::string::npos)
        << "Cancel must validate the owning scene generation before asking the current scene to destroy the transient root.";
    ASSERT_NE(cleanupOffset, std::string::npos);
    ASSERT_NE(destroyOffset, std::string::npos);
    EXPECT_LT(cleanupOffset, tokenCheckOffset)
        << "Cancel should still release prefab/resource ownership for stale-scene transient roots before dropping the pointer.";
    EXPECT_LT(tokenCheckOffset, destroyOffset);
}

TEST(EditorLaunchArgsTests, SceneViewDeliveredAndMovePathsValidateRootSceneTokenBeforeDereference)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");

    const auto handleStart = source.find("void Editor::Panels::SceneView::HandleViewportAssetDragDrop()");
    ASSERT_NE(handleStart, std::string::npos);
    const auto handleEnd = source.find("void Editor::Panels::SceneView::UpdateActivePrefabDragInstance", handleStart);
    ASSERT_NE(handleEnd, std::string::npos);
    const auto handleBody = source.substr(handleStart, handleEnd - handleStart);
    const auto deliveredOffset = handleBody.find("payloadView.delivered");
    ASSERT_NE(deliveredOffset, std::string::npos);
    const auto deliveredTokenCheck = handleBody.find("!IsActivePrefabDragSceneTokenCurrent(*m_activeDraggedPrefabRootSceneToken)", deliveredOffset);
    const auto deliveredRootAlive = handleBody.find("m_activeDraggedPrefabRoot->IsAlive()", deliveredOffset);
    ASSERT_NE(deliveredTokenCheck, std::string::npos);
    ASSERT_NE(deliveredRootAlive, std::string::npos);
    EXPECT_LT(deliveredTokenCheck, deliveredRootAlive)
        << "Mouse release must validate the root's scene token before checking a root pointer from a previous scene generation.";

    const auto updateStart = source.find("void Editor::Panels::SceneView::UpdateActivePrefabDragInstance");
    ASSERT_NE(updateStart, std::string::npos);
    const auto updateEnd = source.find("void Editor::Panels::SceneView::TryRefreshActivePrefabDragHotCacheKey", updateStart);
    ASSERT_NE(updateEnd, std::string::npos);
    const auto updateBody = source.substr(updateStart, updateEnd - updateStart);
    const auto moveTokenCheck = updateBody.find("!IsActivePrefabDragSceneTokenCurrent(*m_activeDraggedPrefabRootSceneToken)");
    const auto moveRootAlive = updateBody.find("m_activeDraggedPrefabRoot->IsAlive()");
    ASSERT_NE(moveTokenCheck, std::string::npos);
    ASSERT_NE(moveRootAlive, std::string::npos);
    EXPECT_LT(moveTokenCheck, moveRootAlive)
        << "Per-frame movement must validate the scene token before touching the transient root.";
}

TEST(EditorLaunchArgsTests, SceneViewMovePathCancelsStaleTransientRootBeforeDroppingPointer)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto updateStart = source.find("void Editor::Panels::SceneView::UpdateActivePrefabDragInstance");
    ASSERT_NE(updateStart, std::string::npos);
    const auto updateEnd = source.find("void Editor::Panels::SceneView::TryRefreshActivePrefabDragHotCacheKey", updateStart);
    ASSERT_NE(updateEnd, std::string::npos);
    const auto body = source.substr(updateStart, updateEnd - updateStart);

    const auto staleTokenCheck = body.find("!IsActivePrefabDragSceneTokenCurrent(*m_activeDraggedPrefabRootSceneToken)");
    ASSERT_NE(staleTokenCheck, std::string::npos);
    const auto cancelAfterStaleToken = body.find("CancelActivePrefabDragInstance()", staleTokenCheck);
    const auto clearAfterStaleToken = body.find("ClearActivePrefabDragState()", staleTokenCheck);
    ASSERT_NE(cancelAfterStaleToken, std::string::npos)
        << "A stale-scene transient root must release prefab/resource ownership before SceneView drops its pointer.";
    if (clearAfterStaleToken != std::string::npos)
        EXPECT_LT(cancelAfterStaleToken, clearAfterStaleToken);
}

TEST(EditorLaunchArgsTests, SceneViewReleasePathCancelsStaleTransientRootBeforeDroppingPointer)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto handleStart = source.find("void Editor::Panels::SceneView::HandleViewportAssetDragDrop()");
    ASSERT_NE(handleStart, std::string::npos);
    const auto handleEnd = source.find("void Editor::Panels::SceneView::UpdateActivePrefabDragInstance", handleStart);
    ASSERT_NE(handleEnd, std::string::npos);
    const auto body = source.substr(handleStart, handleEnd - handleStart);

    const auto deliveredOffset = body.find("payloadView.delivered");
    ASSERT_NE(deliveredOffset, std::string::npos);
    const auto staleTokenCheck = body.find(
        "!IsActivePrefabDragSceneTokenCurrent(*m_activeDraggedPrefabRootSceneToken)",
        deliveredOffset);
    ASSERT_NE(staleTokenCheck, std::string::npos);
    const auto branchEnd = body.find("else if (m_activeDraggedPrefabRoot != nullptr", staleTokenCheck);
    ASSERT_NE(branchEnd, std::string::npos);
    const auto staleBranch = body.substr(staleTokenCheck, branchEnd - staleTokenCheck);

    EXPECT_NE(staleBranch.find("CancelActivePrefabDragInstance()"), std::string::npos)
        << "A mouse-release stale-scene transient root must release prefab/resource ownership before SceneView drops its pointer.";
    EXPECT_EQ(staleBranch.find("ClearActivePrefabDragState()"), std::string::npos)
        << "Release should not bypass CancelActivePrefabDragInstance for stale transient roots.";
}

TEST(EditorLaunchArgsTests, SceneViewHoverDragKeepsTransientPrefabRootHiddenUntilReady)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto updateStart = source.find("void Editor::Panels::SceneView::UpdateActivePrefabDragInstance");
    ASSERT_NE(updateStart, std::string::npos);
    const auto updateEnd = source.find("void Editor::Panels::SceneView::ClearActivePrefabDragState", updateStart);
    ASSERT_NE(updateEnd, std::string::npos);
    const auto body = source.substr(updateStart, updateEnd - updateStart);

    EXPECT_NE(body.find("hideRootUntilRendererResourcesReady = true"), std::string::npos)
        << "Hover-drag transient prefabs should stay hidden until renderer resources finish resolving.";
    EXPECT_NE(body.find("keepRootRenderingSuppressedOnFailure = true"), std::string::npos)
        << "Hover-drag transient prefabs must not reveal a partially bound generated model if renderer resource resolution fails.";
}

TEST(EditorLaunchArgsTests, SceneViewPendingRendererResourceProxyUsesRegistryStateNotHierarchyScan)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto updateStart = source.find("void Editor::Panels::SceneView::UpdateActivePrefabDragInstance");
    ASSERT_NE(updateStart, std::string::npos);
    const auto updateEnd = source.find("void Editor::Panels::SceneView::TryRefreshActivePrefabDragHotCacheKey", updateStart);
    ASSERT_NE(updateEnd, std::string::npos);
    const auto body = source.substr(updateStart, updateEnd - updateStart);

    EXPECT_NE(body.find("EDITOR_CONTEXT(prefabInstanceRegistry).GetPresentation"), std::string::npos)
        << "Drag hover must query renderer-resource readiness from registry state instead of scanning a large prefab hierarchy every frame.";
    EXPECT_EQ(body.find("AnyPrefabDragRendererSuppressed"), std::string::npos)
        << "Recursive root scans can reintroduce large-prefab hover hitches.";
}

TEST(EditorLaunchArgsTests, SceneViewCancelCleansTransientRootBeforeDroppingStaleSceneToken)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto cancelStart = source.find("void Editor::Panels::SceneView::CancelActivePrefabDragInstance");
    ASSERT_NE(cancelStart, std::string::npos);
    const auto cancelEnd = source.find("bool Editor::Panels::SceneView::CommitActivePrefabDragInstance", cancelStart);
    ASSERT_NE(cancelEnd, std::string::npos);
    const auto body = source.substr(cancelStart, cancelEnd - cancelStart);

    const auto staleTokenCheck = body.find("!IsActivePrefabDragSceneTokenCurrent(*rootSceneToken)");
    const auto cleanup = body.find("CleanupPrefabInstanceMarkedDestroy");
    const auto markDestroy = body.find("root->MarkAsDestroy()");
    ASSERT_NE(staleTokenCheck, std::string::npos);
    ASSERT_NE(cleanup, std::string::npos);
    ASSERT_NE(markDestroy, std::string::npos);
    EXPECT_LT(cleanup, staleTokenCheck)
        << "Even stale-scene transient roots must release prefab/resource ownership before SceneView drops its pointer.";
    EXPECT_GT(markDestroy, staleTokenCheck)
        << "Stale-scene transient roots should be marked for destruction instead of silently orphaned.";
}

TEST(EditorLaunchArgsTests, SceneViewHoverDragForcesStaticFrameRenderWhileActive)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = source.find("bool Editor::Panels::SceneView::ShouldForceStaticFrameRender() const");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("bool Editor::Panels::SceneView::RequiresSynchronizedRetiredFramePresentation() const", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("m_activeDraggedPrefabPayload.has_value()"), std::string::npos)
        << "Active prefab drags must keep the Scene View rendering every frame so the root follows the cursor.";
    EXPECT_NE(body.find("m_activeDraggedPrefabCommitPending"), std::string::npos)
        << "A released cold prefab drag must keep rendering until the background hot-cache instance can be committed.";
}

TEST(EditorLaunchArgsTests, SceneViewValidationReadbackWaitsForVisibleSceneLoadResources)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = source.find("void Editor::Panels::SceneView::TryWriteValidationReadback()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("const auto width = static_cast<uint32_t>", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(
        body.find("ShouldWaitForSceneViewValidationReadbackSceneLoadResources"),
        std::string::npos)
        << "Scene View readback validation should skip the placeholder frame but capture after scene-load objects become visible.";
    EXPECT_NE(body.find("GetVisibleSceneLoadRendererResourceResolutionObjectCount"), std::string::npos)
        << "Validation readback should use progressive reveal state instead of waiting for the full mesh/material long tail.";
    EXPECT_NE(body.find("m_validationReadbackReadyFrames = 0u"), std::string::npos);
}

TEST(EditorLaunchArgsTests, SceneViewValidationReadbackCancelsSceneLoadResourceResolutionBeforeClose)
{
    const auto sceneViewSource = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = sceneViewSource.find("void Editor::Panels::SceneView::TryWriteValidationReadback()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = sceneViewSource.find("void Editor::Panels::SceneView::DrawViewportOverlay()", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = sceneViewSource.substr(functionStart, functionEnd - functionStart);

    const auto cancelCall = body.find("CancelSceneLoadRendererResourceResolution()");
    const auto closeCall = body.find("SetShouldClose(true)", cancelCall);
    ASSERT_NE(cancelCall, std::string::npos);
    ASSERT_NE(closeCall, std::string::npos)
        << "Validation shutdown should cancel scene-load renderer-resource work before closing so automated captures do not wait for the full resource long tail.";

    const auto actionsHeader = ReadTextFile("Project/Editor/Core/EditorActions.h");
    EXPECT_NE(actionsHeader.find("void CancelSceneLoadRendererResourceResolution();"), std::string::npos);

    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto cancelStart = actionsSource.find("void NLS::Editor::Core::CancelSceneLoadRendererResourceResolution()");
    ASSERT_NE(cancelStart, std::string::npos);
    const auto cancelEnd = actionsSource.find("Editor::Core::EditorActions::EditorActions", cancelStart);
    ASSERT_NE(cancelEnd, std::string::npos);
    const auto cancelBody = actionsSource.substr(cancelStart, cancelEnd - cancelStart);
    EXPECT_NE(cancelBody.find("state->cancelled.store(true"), std::string::npos);
    EXPECT_NE(cancelBody.find("state->completed.store(true"), std::string::npos);
    EXPECT_NE(cancelBody.find("state->remainingTasks.clear()"), std::string::npos);
    EXPECT_NE(cancelBody.find("state->inFlightTasks.clear()"), std::string::npos);
    EXPECT_NE(cancelBody.find("CancelRendererResourceMeshLoads(*state, true)"), std::string::npos)
        << "Validation shutdown should also request cancellation of shared scene-load mesh artifact jobs.";
    EXPECT_NE(cancelBody.find("FinishJob"), std::string::npos);
}

TEST(EditorLaunchArgsTests, SceneViewValidationReadbackCloseBypassesUnsavedScenePrompt)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = actionsSource.find("bool Editor::Core::EditorActions::PromptSaveCurrentSceneIfDirty()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = actionsSource.find(
        "std::optional<std::string> Editor::Core::EditorActions::SelectBuildFolder()",
        functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = actionsSource.substr(functionStart, functionEnd - functionStart);

    const auto readbackOutput = body.find("editorValidationSceneReadbackOutput");
    const auto readbackSummary = body.find("editorValidationSceneReadbackSummary");
    const auto messageBox = body.find("Dialogs::MessageBox");
    ASSERT_NE(readbackOutput, std::string::npos);
    ASSERT_NE(readbackSummary, std::string::npos);
    ASSERT_NE(messageBox, std::string::npos);
    EXPECT_LT(readbackOutput, messageBox);
    EXPECT_LT(readbackSummary, messageBox)
        << "Automated Scene View readback validation must close without showing an unsaved-scene dialog.";
}

TEST(EditorLaunchArgsTests, SceneLoadMeshArtifactLoadsAreOpportunistic)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = actionsSource.find("bool StartMeshArtifactLoad");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = actionsSource.find("enum class RendererResourceResolutionTaskPopPreference", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto functionBody = actionsSource.substr(functionStart, functionEnd - functionStart);

    const auto sceneLoadBranch = functionBody.find("resolutionState.shareSceneLoadFrameBudget");
    ASSERT_NE(sceneLoadBranch, std::string::npos);
    const auto opportunisticSubmit = functionBody.find("TrackOpportunisticBackgroundTask", sceneLoadBranch);
    const auto requiredSubmit = functionBody.find("TrackBackgroundTask", sceneLoadBranch);
    ASSERT_NE(opportunisticSubmit, std::string::npos)
        << "Scene-load mesh artifact jobs should be discardable during validation shutdown instead of forcing the editor to drain every queued mesh load.";
    ASSERT_NE(requiredSubmit, std::string::npos)
        << "Non-scene-load renderer restoration still needs required work semantics.";
    EXPECT_LT(opportunisticSubmit, requiredSubmit);

    const auto rejectionBranch = functionBody.find("if (!accepted)");
    ASSERT_NE(rejectionBranch, std::string::npos);
    const auto rejectionBodyEnd = functionBody.find("return false;", rejectionBranch);
    ASSERT_NE(rejectionBodyEnd, std::string::npos);
    const auto rejectionBody = functionBody.substr(rejectionBranch, rejectionBodyEnd - rejectionBranch);
    EXPECT_NE(rejectionBody.find("state->completed = true;"), std::string::npos)
        << "Rejected opportunistic scene-load mesh jobs must be visible to the in-flight poller as completed/retryable; completed=false leaves mesh tasks pending forever.";
}

TEST(EditorLaunchArgsTests, SceneViewValidationReadbackForcesRenderOnlyAfterSceneLoadResourcesDrain)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = source.find("bool Editor::Panels::SceneView::ShouldForceStaticFrameRender() const");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("bool Editor::Panels::SceneView::ShouldDeferRenderFrame() const", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    const auto readbackRequest = body.find("editorValidationSceneReadbackOutput");
    const auto pendingDrainGate = body.find("GetPendingSceneLoadRendererResourceResolutionTaskCount() == 0u");
    ASSERT_NE(readbackRequest, std::string::npos);
    ASSERT_NE(pendingDrainGate, std::string::npos);
    EXPECT_GT(pendingDrainGate, readbackRequest)
        << "Readback validation should not force every Scene View frame while scene-load resources are still draining.";
}

TEST(EditorLaunchArgsTests, SceneViewUpdateCommitsColdPrefabDragAfterHotCacheReady)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = source.find("void Editor::Panels::SceneView::Update(float p_deltaTime)");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("Editor::Core::EGizmoOperation Editor::Panels::SceneView::GetCurrentGizmoOperation", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("UpdateActivePrefabDragInstance(*m_activeDraggedPrefabPayload)"), std::string::npos)
        << "Cold prefab drags need Update() to keep polling the hot cache even when drag/drop callbacks stop.";
    EXPECT_NE(body.find("m_activeDraggedPrefabCommitPending"), std::string::npos)
        << "Scene View must remember that the mouse was released before the prefab became ready.";
    EXPECT_NE(body.find("CommitActivePrefabDragInstance()"), std::string::npos)
        << "Once the background hot-cache instance appears, Update() should commit it at the last resolved cursor position.";
}

TEST(EditorLaunchArgsTests, SceneViewClearActivePrefabDragStateClearsCachedHandle)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = source.find("void Editor::Panels::SceneView::ClearActivePrefabDragState()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void Editor::Panels::SceneView::CancelActivePrefabDragInstance", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("m_activeDraggedPrefabCommitPending = false"), std::string::npos);
    EXPECT_NE(body.find("m_activeDraggedPrefabAssetPath.clear()"), std::string::npos);
    EXPECT_NE(body.find("m_activeDraggedPrefabSubAssetKey.clear()"), std::string::npos);
    EXPECT_NE(body.find("m_activeDraggedPrefabAssetId = {}"), std::string::npos);
    EXPECT_NE(body.find("m_activeDraggedPrefabAssetType = NLS::Core::Assets::AssetType::Unknown"), std::string::npos);
    EXPECT_NE(body.find("m_activeDraggedPrefabDropSceneToken.reset()"), std::string::npos);
    EXPECT_NE(body.find("m_activeDraggedPrefabRootSceneToken.reset()"), std::string::npos);
    EXPECT_NE(body.find("m_activeDraggedPrefabDropPlacement.reset()"), std::string::npos);
    EXPECT_NE(body.find("m_activeDraggedPrefabHotCacheKeyBuildAttempted = false"), std::string::npos);
}

TEST(EditorLaunchArgsTests, HotCacheOnlyPrefabDropDoesNotReResolvePayloadPerHoverFrame)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/EditorAssetDragDropBridge.cpp");
    const auto functionStart = source.find("EditorAssetDragDropBridge::TryDropImportedAssetHandleFromHotCacheIntoHierarchy(");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropImportedAssetHandleIntoHierarchyBlocking", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("TryGetImportedPrefabHotCache"), std::string::npos)
        << "The cached handle overload should only instantiate from an already-prewarmed hot-cache entry.";
    EXPECT_NE(body.find("MakePendingImportedPrefabResult"), std::string::npos)
        << "Cold prefab drags should report pending instead of blocking or failing permanently.";
    EXPECT_EQ(body.find("EditorAssetDragPayload payload"), std::string::npos)
        << "Per-frame Scene View hover drops should not rebuild payloads and re-run metadata resolution.";
    EXPECT_EQ(body.find("ResolveImportedAssetHandleForPayload"), std::string::npos)
        << "The cached handle overload should use the handle cached at drag start.";
    EXPECT_EQ(body.find("BuildUnifiedPrefabLoadKey"), std::string::npos)
        << "Scene View hover frames should use the prefab hot-cache key computed when the drag payload first entered the viewport.";
    EXPECT_NE(body.find("hotCacheKey.source.sourceAssetPath"), std::string::npos)
        << "The cached handle overload must still verify the key belongs to the cached drag handle.";
    EXPECT_NE(body.find("hotCacheKey.source.prefabSubAssetKey"), std::string::npos);
    EXPECT_NE(body.find("hotCacheKey.source.sourceAssetId"), std::string::npos);
    EXPECT_NE(body.find("hotCacheKey.source.assetType"), std::string::npos);
    EXPECT_NE(body.find("!hotCacheKey.rendererArtifactReadinessRequired"), std::string::npos)
        << "The explicit-key overload must reject graph-only hot-cache keys by mode, not by stamp content.";
    EXPECT_EQ(body.find("hotCacheKey.stamps.rendererArtifactStamp.empty()"), std::string::npos)
        << "An empty renderer artifact stamp can be a valid renderer-ready key when the manifest has no stampable renderer artifacts.";
    EXPECT_EQ(body.find("hotCacheKey.rendererArtifactStamp.empty()"), std::string::npos)
        << "The explicit-key overload should trust the formal hot-cache identity, not stamp non-emptiness.";
    EXPECT_NE(body.find("IsImportedPrefabHotCacheKeyCurrent"), std::string::npos)
        << "Renderer-ready hover instantiation must reject stale hot-cache keys before creating a preview root.";
}

TEST(EditorLaunchArgsTests, SceneViewHotCacheKeyRefreshPollsMemoryOnly)
{
    const auto header = ReadTextFile("Project/Editor/Panels/SceneView.h");
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");

    EXPECT_NE(header.find("m_activeDraggedPrefabHotCacheKeyBuildAttempted"), std::string::npos);

    const auto functionStart = source.find("void Editor::Panels::SceneView::TryRefreshActivePrefabDragHotCacheKey()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void Editor::Panels::SceneView::ClearActivePrefabDragState", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("m_activeDraggedPrefabHotCacheKeyBuildAttempted"), std::string::npos);
    EXPECT_NE(body.find("TryFindImportedPrefabHotCacheKey"), std::string::npos)
        << "Scene View drag hover should recover prepared keys by scanning the in-memory hot cache only.";
    EXPECT_NE(body.find("UnifiedPrefabReadiness::MeshMaterialTextureReady"), std::string::npos)
        << "Renderer-ready hot-cache entries should be preferred when available.";
    EXPECT_NE(body.find("UnifiedPrefabReadiness::PrefabGraphOnly"), std::string::npos)
        << "Graph-only entries may drive transient hover preview while renderer artifacts are still resolving.";
    EXPECT_EQ(body.find("BuildUnifiedPrefabLoadKey"), std::string::npos)
        << "Scene View hover frames must not synchronously touch meta files or manifests.";
    EXPECT_EQ(body.find("AssetMeta::Load"), std::string::npos);
    EXPECT_EQ(body.find("LoadFastManifest"), std::string::npos);
}

TEST(EditorLaunchArgsTests, BridgeFindsImportedPrefabHotCacheKeyWithoutDiskKeyBuild)
{
    const auto header = ReadTextFile("Project/Editor/Assets/EditorAssetDragDropBridge.h");
    const auto source = ReadTextFile("Project/Editor/Assets/EditorAssetDragDropBridge.cpp");

    EXPECT_NE(header.find("TryFindImportedPrefabHotCacheKey"), std::string::npos);

    const auto functionStart = source.find("std::optional<UnifiedPrefabLoadKey> EditorAssetDragDropBridge::TryFindImportedPrefabHotCacheKey");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("bool EditorAssetDragDropBridge::IsImportedPrefabHotCacheKeyCurrent", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("ImportedPrefabHotCacheMutex"), std::string::npos);
    EXPECT_NE(body.find("ImportedPrefabHotCacheState"), std::string::npos);
    EXPECT_NE(body.find("entry.key.source.sourceAssetPath"), std::string::npos);
    EXPECT_NE(body.find("entry.key.source.prefabSubAssetKey"), std::string::npos);
    EXPECT_NE(body.find("entry.key.source.sourceAssetId"), std::string::npos);
    EXPECT_NE(body.find("entry.key.source.assetType"), std::string::npos);
    EXPECT_NE(body.find("requiredReadiness"), std::string::npos);
    EXPECT_NE(body.find("entry.key.rendererArtifactReadinessRequired != rendererReadinessRequired"), std::string::npos)
        << "Scene View key recovery must select the requested readiness mode explicitly.";
    EXPECT_NE(body.find("entry.key.runtimeCacheIdentity.empty()"), std::string::npos)
        << "Scene View key recovery must only return formal prefab keys that can be looked up exactly.";
    EXPECT_EQ(body.find("entry.key.stamps.rendererArtifactStamp.empty()"), std::string::npos)
        << "An empty renderer artifact stamp can be valid for renderer-ready prefabs and must not block drag recovery.";
    EXPECT_EQ(body.find("entry.key.rendererArtifactStamp.empty()"), std::string::npos)
        << "Hot-cache key recovery should use identity/source matching instead of stamp non-emptiness.";
    EXPECT_EQ(body.find("BuildUnifiedPrefabLoadKey"), std::string::npos)
        << "Hot-cache key recovery is a per-frame poll path and must not touch meta files or manifests.";
    EXPECT_EQ(body.find("AssetMeta::Load"), std::string::npos);
    EXPECT_EQ(body.find("LoadFastManifest"), std::string::npos);
}

TEST(EditorLaunchArgsTests, SceneViewCommitCancelsStalePrefabHotCacheKey)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = source.find("bool Editor::Panels::SceneView::CommitActivePrefabDragInstance()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("Editor::Core::EditorActions::SceneMutationToken Editor::Panels::SceneView::CaptureActivePrefabDragSceneToken", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("IsImportedPrefabHotCacheKeyCurrent"), std::string::npos)
        << "Final commit must reject a prefab hot-cache key that became stale while the user was dragging.";
    EXPECT_NE(body.find("CancelActivePrefabDragInstance()"), std::string::npos)
        << "A stale hot-cache root must be removed instead of becoming a committed scene object.";
}

TEST(EditorLaunchArgsTests, BridgePrefabHotCacheKeyCurrentCheckUsesCurrentFinalDropKey)
{
    const auto header = ReadTextFile("Project/Editor/Assets/EditorAssetDragDropBridge.h");
    const auto source = ReadTextFile("Project/Editor/Assets/EditorAssetDragDropBridge.cpp");

    EXPECT_NE(header.find("IsImportedPrefabHotCacheKeyCurrent"), std::string::npos);

    const auto functionStart = source.find("bool EditorAssetDragDropBridge::IsImportedPrefabHotCacheKeyCurrent");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("bool EditorAssetDragDropBridge::PreloadPreparedPrefabHotCache", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("UnifiedPrefabLoadMode::FinalDrop"), std::string::npos);
    EXPECT_NE(body.find("UnifiedPrefabReadiness::MeshMaterialTextureReady"), std::string::npos);
    EXPECT_NE(body.find("BuildUnifiedPrefabLoadKey(request)"), std::string::npos);
    EXPECT_NE(body.find("currentKey->runtimeCacheIdentity == hotCacheKey.runtimeCacheIdentity"), std::string::npos);
    EXPECT_EQ(body.find("hotCacheKey.rendererArtifactReadinessRequired"), std::string::npos)
        << "Final commit freshness must not accept graph-only preview keys as renderer-ready scene drops.";
    EXPECT_EQ(body.find("UnifiedPrefabReadiness::PrefabGraphOnly"), std::string::npos)
        << "Graph-only preview keys are transient and must not satisfy final-drop freshness.";
}

TEST(EditorLaunchArgsTests, SceneViewCommitRejectsGraphOnlyPreviewKeyBeforeFinalizing)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = source.find("bool Editor::Panels::SceneView::CommitActivePrefabDragInstance()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("Editor::Core::EditorActions::SceneMutationToken Editor::Panels::SceneView::CaptureActivePrefabDragSceneToken", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    const auto graphOnlyGuard = body.find("!m_activeDraggedPrefabHotCacheKey->rendererArtifactReadinessRequired");
    ASSERT_NE(graphOnlyGuard, std::string::npos)
        << "Scene View final release must not convert graph-only hover previews into permanent scene objects.";
    const auto cancelAfterGuard = body.find("CancelActivePrefabDragInstance()", graphOnlyGuard);
    ASSERT_NE(cancelAfterGuard, std::string::npos)
        << "A graph-only preview without a renderer-ready replacement should be removed on final release.";
    const auto formalDropAfterCancel = body.find("TryDropImportedAssetHandleFromHotCacheIntoHierarchy", cancelAfterGuard);
    ASSERT_NE(formalDropAfterCancel, std::string::npos)
        << "If renderer-ready data exists, Scene View must create a formal instance instead of permanentizing the graph-only preview root.";
    EXPECT_NE(body.find("rendererReadyHotCacheKey"), std::string::npos)
        << "The renderer-ready key used for formal commit must be kept separate from the graph-only preview key.";
    const auto finalize = body.find("root->SetEditorTransient(false)");
    ASSERT_NE(finalize, std::string::npos);
    EXPECT_LT(formalDropAfterCancel, finalize)
        << "Graph-only preview rejection must happen before clearing the transient flag.";
}

TEST(EditorLaunchArgsTests, UnifiedPrefabRuntimeCacheIdentitySeparatesRendererReadinessMode)
{
    const auto header = ReadTextFile("Project/Editor/Assets/EditorAssetDragDropBridge.h");
    const auto source = ReadTextFile("Project/Editor/Assets/EditorAssetDragDropBridge.cpp");

    EXPECT_NE(header.find("rendererArtifactReadinessRequired"), std::string::npos)
        << "The hot-cache key needs an explicit readiness bit so an empty renderer stamp can still mean renderer-ready.";

    const auto functionStart = source.find("std::string BuildUnifiedPrefabRuntimeCacheIdentity");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("PreparedPrefabCacheFreshnessRecord BuildPreparedPrefabCacheFreshnessRecord", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("rendererReadiness@"), std::string::npos)
        << "Graph-only and renderer-ready prefab hot-cache entries must not share an identity when the renderer stamp is empty.";
    EXPECT_NE(body.find("rendererArtifactReadinessRequired ? \"required\" : \"graph\""), std::string::npos);
    EXPECT_LT(body.find("rendererReadiness@"), body.find("|renderer@"))
        << "The readiness mode should be part of the renderer identity section, before the renderer artifact stamp value.";
}

TEST(EditorLaunchArgsTests, SceneViewPickingIgnoresActiveDraggedPrefabRoot)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = source.find("void Editor::Panels::SceneView::HandleGameObjectPicking()");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.size();
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("m_activeDraggedPrefabRoot"), std::string::npos)
        << "Transient prefab drag instances must be filtered out of hover/click picking.";
    EXPECT_NE(body.find("IsEditorTransient()"), std::string::npos)
        << "Filtering should also cover transient children created for the active prefab drag.";
    EXPECT_NE(body.find("m_highlightedGameObject = nullptr"), std::string::npos);
}

TEST(EditorLaunchArgsTests, HierarchyImportedPrefabDropHidesGeneratedRootsUntilRendererResourcesAreReady)
{
    const auto source = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = source.find("Engine::GameObject* NLS::Editor::Core::EditorActions::CreateGameObjectFromImportedPrefabArtifact(");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void NLS::Editor::Core::EditorActions::CompletePendingAssetDrop", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("QueuePrefabInstanceAssetResolution("), std::string::npos)
        << "Hierarchy prefab drops should still queue renderer resource resolution for imported prefabs.";
    EXPECT_NE(body.find("result.dragDrop.deferredAssetReferenceResolutionRequested"), std::string::npos)
        << "The hide-root decision should follow the same deferred-resolution branch used by other prefab drops.";
    EXPECT_NE(body.find("hideRootUntilRendererResourcesReady = true"), std::string::npos)
        << "Hierarchy prefab drops should hide the generated root while renderer resources are pending.";
}

TEST(EditorLaunchArgsTests, GeneratedModelDragDropResolutionOptionsDoNotRevealPartialResources)
{
    const auto source = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = source.find(
        "NLS::Editor::Core::PrefabInstanceAssetResolutionOptions MakeDragDropPrefabInstanceAssetResolutionOptions");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void ApplyRendererResourceMaterialPathHints", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("dragDrop.deferredAssetReferenceResolutionRequested"), std::string::npos)
        << "The final drop options should only suppress roots for generated-model drops that requested deferred renderer resource resolution.";
    EXPECT_NE(body.find("hideRootUntilRendererResourcesReady = true"), std::string::npos)
        << "Final generated-model drops must not reveal graph-only or partially bound renderer resources.";
    EXPECT_NE(body.find("keepRootRenderingSuppressedOnFailure = true"), std::string::npos)
        << "A failed final renderer resolution must not leave an incomplete generated prefab visible in the scene.";
}

TEST(EditorLaunchArgsTests, SceneRestorePrefabSourceLookupFallsBackBeforeMarkingMissing)
{
    const auto source = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto restoreStart = source.find("bool Editor::Core::EditorActions::RestorePrefabInstancesForCurrentSceneFromDisk()");
    ASSERT_NE(restoreStart, std::string::npos);
    const auto restoreEnd = source.find("void Editor::Core::EditorActions::LoadSceneFromDisk", restoreStart);
    ASSERT_NE(restoreEnd, std::string::npos);
    const auto body = source.substr(restoreStart, restoreEnd - restoreStart);

    const auto fallbackLookup = body.find("GUIDToAssetPath(assetId.ToString())");
    const auto missingMark = body.find("sourceMissingPrefabSources.insert(cacheKey)");
    ASSERT_NE(fallbackLookup, std::string::npos)
        << "Scene restore must use the source asset database as a fallback when ArtifactDB lacks an UpToDate source path.";
    ASSERT_NE(missingMark, std::string::npos);
    EXPECT_LT(fallbackLookup, missingMark)
        << "ArtifactDB miss should not mark a prefab source missing before source metadata lookup has a chance to resolve it.";
}

TEST(EditorLaunchArgsTests, SceneViewCurrentDragPrefabPrewarmIsRequiredBackgroundWork)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = source.find("bool ScheduleSceneViewImportedPrefabPreloadOnce");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void HashSceneViewCacheValue", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("Schedule("), std::string::npos)
        << "The prefab currently under the mouse should use the main JobSystem queue instead of the single-worker background queue.";
    EXPECT_NE(body.find("JobPriority::High"), std::string::npos)
        << "Current drag loading must be promoted above normal editor work so it is not delayed behind visible-item prewarm.";
    EXPECT_EQ(body.find("TrackBackgroundTask"), std::string::npos)
        << "Current drag loading should not wait behind the single-worker background queue.";
    EXPECT_EQ(body.find("TrackOpportunisticBackgroundTask"), std::string::npos)
        << "Current drag loading should not be rejected or delayed as opportunistic cache warming.";
}

TEST(EditorLaunchArgsTests, SharedImportedPrefabDropPassesCachedRendererDependencyTemplatesToResolution)
{
    const auto dragDropHeader = ReadTextFile("Project/Editor/Assets/AssetDragDropWorkflow.h");
    EXPECT_NE(dragDropHeader.find("rendererDependencyTemplates"), std::string::npos)
        << "Drag/drop results should carry renderer dependency templates loaded with the prefab hot cache.";
    EXPECT_NE(
        dragDropHeader.find("std::shared_ptr<const std::vector<ImportedPrefabRendererDependencyTemplate>>"),
        std::string::npos)
        << "Large prefab drags should pass cached renderer dependency templates by shared immutable handle, not deep-copy vectors.";

    const auto bridgeSource = ReadTextFile("Project/Editor/Assets/EditorAssetDragDropBridge.cpp");
    const auto instantiateStart = bridgeSource.find(
        "EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::InstantiateImportedAsset");
    ASSERT_NE(instantiateStart, std::string::npos);
    const auto instantiateEnd = bridgeSource.find(
        "EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropModelAssetIntoHierarchy",
        instantiateStart);
    ASSERT_NE(instantiateEnd, std::string::npos);
    const auto instantiateBody = bridgeSource.substr(instantiateStart, instantiateEnd - instantiateStart);
    EXPECT_NE(instantiateBody.find("TryGetImportedPrefabRendererDependencyTemplates"), std::string::npos)
        << "Hot-cache drops should attach cached renderer dependency templates to the drag/drop result.";

    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto queueStart = actionsSource.find("void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution");
    ASSERT_NE(queueStart, std::string::npos);
    const auto queueEnd = actionsSource.find("bool Editor::Core::EditorActions::DestroyGameObject", queueStart);
    ASSERT_NE(queueEnd, std::string::npos);
    const auto queueBody = actionsSource.substr(queueStart, queueEnd - queueStart);
    EXPECT_NE(queueBody.find("rendererDependencyTemplates"), std::string::npos)
        << "Resource resolution should accept cached templates instead of rebuilding them from the prefab each time.";
    EXPECT_NE(queueBody.find("BuildPrefabAssetResolutionTasksFromTemplates"), std::string::npos);
    EXPECT_NE(queueBody.find("*options.rendererDependencyTemplates"), std::string::npos)
        << "Resolution should consume the shared immutable template vector without copying it first.";

    const auto collectStart = actionsSource.find("void CollectPrefabAssetResolutionTasks");
    ASSERT_NE(collectStart, std::string::npos);
    const auto collectEnd = actionsSource.find("bool TrackRendererResourceAsyncInterest", collectStart);
    ASSERT_NE(collectEnd, std::string::npos);
    const auto collectBody = actionsSource.substr(collectStart, collectEnd - collectStart);
    EXPECT_NE(collectBody.find("cachedRendererDependencyTemplates"), std::string::npos)
        << "Collecting resolution tasks should prefer already cached templates when present.";
}

TEST(EditorLaunchArgsTests, SceneRestoreGeneratedModelPrefabsAreNotMarkedMissingBeforeDeferredResolution)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto restoreStart = actionsSource.find("bool Editor::Core::EditorActions::RestorePrefabInstancesForCurrentSceneFromDisk()");
    ASSERT_NE(restoreStart, std::string::npos);
    const auto restoreEnd = actionsSource.find("void Editor::Core::EditorActions::LoadSceneFromDisk", restoreStart);
    ASSERT_NE(restoreEnd, std::string::npos);
    const auto restoreBody = actionsSource.substr(restoreStart, restoreEnd - restoreStart);

    const auto modelSceneBranch = restoreBody.find("const bool generatedModelScene =");
    ASSERT_NE(modelSceneBranch, std::string::npos);
    EXPECT_NE(
        restoreBody.find(
            "NLS::Core::Assets::InferAssetType(absoluteSourcePath) == NLS::Core::Assets::AssetType::ModelScene",
            modelSceneBranch),
        std::string::npos);
    const auto generatedModelCountBranch = restoreBody.find("if (generatedModelScene)", modelSceneBranch);
    ASSERT_NE(generatedModelCountBranch, std::string::npos);
    EXPECT_NE(
        restoreBody.find("++deferredGeneratedModelPrefabLoads;", generatedModelCountBranch),
        std::string::npos)
        << "Generated model scene-prefab restores should still be counted as deferred renderer-ready work.";

    const auto artifactLoadBegin = restoreBody.find("const auto artifactLoadBegin = std::chrono::steady_clock::now();", modelSceneBranch);
    ASSERT_NE(artifactLoadBegin, std::string::npos)
        << "Generated model scene-prefab restores should continue into graph-only prefab artifact loading.";

    const auto badEarlyReturn = restoreBody.find(
        "prefabArtifactCache.emplace(cacheKey, nullptr);\n                return nullptr;",
        modelSceneBranch);
    EXPECT_EQ(badEarlyReturn, std::string::npos)
        << "Generated model scene-prefab restores must not be marked missing before deferred renderer resource resolution runs.";
}

TEST(EditorLaunchArgsTests, SceneRestoreCachesPrefabArtifactsAcrossInstanceConnections)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto restoreStart = actionsSource.find("bool Editor::Core::EditorActions::RestorePrefabInstancesForCurrentSceneFromDisk()");
    ASSERT_NE(restoreStart, std::string::npos);
    const auto restoreEnd = actionsSource.find("void Editor::Core::EditorActions::LoadSceneFromDisk", restoreStart);
    ASSERT_NE(restoreEnd, std::string::npos);
    const auto restoreBody = actionsSource.substr(restoreStart, restoreEnd - restoreStart);

    const auto restoreCall = restoreBody.find("RestorePrefabInstancesFromSceneDocument");
    ASSERT_NE(restoreCall, std::string::npos);
    const auto cacheLookup = restoreBody.find("prefabArtifactCache.find(cacheKey)", restoreCall);
    const auto artifactLoad = restoreBody.find("LoadSceneRestorePrefabArtifactReady", restoreCall);
    const auto cachePublish = restoreBody.find("prefabArtifactCache.emplace(cacheKey, artifact)", artifactLoad);
    ASSERT_NE(cacheLookup, std::string::npos);
    ASSERT_NE(artifactLoad, std::string::npos);
    ASSERT_NE(cachePublish, std::string::npos);
    EXPECT_LT(cacheLookup, artifactLoad);
    EXPECT_LT(artifactLoad, cachePublish);
}

TEST(EditorLaunchArgsTests, PrefabDragDropTelemetryRecordsPrewarmAndRendererResolutionStages)
{
    const auto telemetryHeader = ReadTextFile("Runtime/Core/Assets/ArtifactLoadTelemetry.h");
    EXPECT_NE(telemetryHeader.find("PrefabVisiblePrewarmSchedule"), std::string::npos);
    EXPECT_NE(telemetryHeader.find("PrefabVisiblePrewarmLoad"), std::string::npos);
    EXPECT_NE(telemetryHeader.find("PrefabUnifiedSharedLoad"), std::string::npos);
    EXPECT_NE(telemetryHeader.find("PrefabRendererTaskBuild"), std::string::npos);
    EXPECT_NE(telemetryHeader.find("PrefabRendererResolutionStep"), std::string::npos);

    const auto assetBrowserSource = ReadTextFile("Project/Editor/Panels/AssetBrowser.cpp");
    const auto scheduleStart = assetBrowserSource.find(
        "void Editor::Panels::AssetBrowser::SchedulePrefabHotCachePreloadForDragPayload");
    ASSERT_NE(scheduleStart, std::string::npos);
    const auto scheduleEnd = assetBrowserSource.find(
        "void Editor::Panels::AssetBrowser::SchedulePrefabHotCachePreloadForVisibleItems",
        scheduleStart);
    ASSERT_NE(scheduleEnd, std::string::npos);
    const auto scheduleBody = assetBrowserSource.substr(scheduleStart, scheduleEnd - scheduleStart);
    EXPECT_NE(scheduleBody.find("PrefabVisiblePrewarmSchedule"), std::string::npos)
        << "Visible prefab cells should report whether hot-cache prewarm work was queued or gated.";
    EXPECT_NE(scheduleBody.find("Prefab hot-cache prewarm queued"), std::string::npos)
        << "Users need a Console signal that a visible prefab has been queued for background prewarm.";
    EXPECT_NE(scheduleBody.find("Prefab hot-cache prewarm started"), std::string::npos)
        << "Users need a Console signal that background prewarm work actually started.";
    EXPECT_NE(scheduleBody.find("Prefab hot-cache prewarm "), std::string::npos)
        << "Users need a Console signal when prewarm finishes ready or not ready.";
    EXPECT_NE(scheduleBody.find("elapsedMs="), std::string::npos)
        << "Prewarm completion logs should include elapsed time for diagnosing slow assets.";

    const auto bridgeSource = ReadTextFile("Project/Editor/Assets/EditorAssetDragDropBridge.cpp");
    const auto sharedLoadStart = bridgeSource.find(
        "UnifiedPrefabSharedLoadResult EditorAssetDragDropBridge::LoadUnifiedPrefabShared");
    ASSERT_NE(sharedLoadStart, std::string::npos);
    const auto sharedLoadEnd = bridgeSource.find(
        "bool EditorAssetDragDropBridge::PreloadImportedAssetHandlePrefabHotCache",
        sharedLoadStart);
    ASSERT_NE(sharedLoadEnd, std::string::npos);
    const auto sharedLoadBody = bridgeSource.substr(sharedLoadStart, sharedLoadEnd - sharedLoadStart);
    EXPECT_NE(sharedLoadBody.find("PrefabUnifiedSharedLoad"), std::string::npos)
        << "Shared prefab loads need one stage timer to distinguish cache lookup from cold graph load.";
    EXPECT_NE(sharedLoadBody.find("CheckArtifactLoadBudget"), std::string::npos);

    const auto preloadStart = sharedLoadEnd;
    const auto preloadEnd = bridgeSource.find(
        "std::string EditorAssetDragDropBridge::NormalizeResourcePath",
        preloadStart);
    ASSERT_NE(preloadEnd, std::string::npos);
    const auto preloadBody = bridgeSource.substr(preloadStart, preloadEnd - preloadStart);
    EXPECT_NE(preloadBody.find("PrefabVisiblePrewarmLoad"), std::string::npos)
        << "Background prewarm must publish whether it finished quickly enough to help first drag.";

    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto queueStart = actionsSource.find(
        "void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution");
    ASSERT_NE(queueStart, std::string::npos);
    const auto queueEnd = actionsSource.find("bool Editor::Core::EditorActions::DestroyGameObject", queueStart);
    ASSERT_NE(queueEnd, std::string::npos);
    const auto queueBody = actionsSource.substr(queueStart, queueEnd - queueStart);
    EXPECT_NE(queueBody.find("PrefabRendererTaskBuild"), std::string::npos)
        << "NewSponza task construction needs telemetry so repeated-drop cost can be separated from binding cost.";

    const auto stepStart = actionsSource.find("void RunRendererResourceResolutionStep");
    ASSERT_NE(stepStart, std::string::npos);
    const auto stepEnd = actionsSource.find("std::vector<NLS::Editor::Core::RendererResourceResolutionQueuePlanEntry>", stepStart);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepBody = actionsSource.substr(stepStart, stepEnd - stepStart);
    EXPECT_NE(stepBody.find("PrefabRendererResolutionStep"), std::string::npos)
        << "Per-frame renderer resource binding needs telemetry to explain mouse-follow stutter.";
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

TEST(EditorLaunchArgsTests, RendererResourceResolutionDoesNotHeadOfLineBlockOnPendingMaterialTasks)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto stepStart = actionsSource.find("void RunRendererResourceResolutionStep");
    ASSERT_NE(stepStart, std::string::npos);
    const auto stepEnd = actionsSource.find(
        "std::vector<NLS::Editor::Core::RendererResourceResolutionQueuePlanEntry>",
        stepStart);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepBody = actionsSource.substr(stepStart, stepEnd - stepStart);

    EXPECT_EQ(
        stepBody.find("state->remainingTasks.push_front(std::move(task));"),
        std::string::npos)
        << "Pending material resource loads must move to in-flight polling instead of blocking all later mesh/material tasks.";
    EXPECT_NE(stepBody.find("state->inFlightTasks.push_back(std::move(task));"), std::string::npos);
    EXPECT_NE(stepBody.find("processedTasksThisFrame"), std::string::npos)
        << "Incomplete in-flight tasks must count against the per-frame work limit.";
    EXPECT_NE(
        stepBody.find("processedTasksThisFrame < kRendererResourceResolutionBindTasksPerFrame"),
        std::string::npos)
        << "The per-frame resource step limit must gate attempted tasks, not only completed binds.";
    EXPECT_NE(stepBody.find("kRendererResourceResolutionInFlightPollTasksPerFrame"), std::string::npos)
        << "Pending in-flight material polls need a separate cap so they cannot consume the whole frame's task budget.";
    EXPECT_NE(stepBody.find("polledInFlightTasksThisFrame"), std::string::npos)
        << "The renderer-resource step should leave room to schedule later mesh/material tasks while earlier async work is still pending.";
    EXPECT_NE(stepBody.find("CountInFlightMeshRendererResourceTasks"), std::string::npos)
        << "Mesh scheduling capacity must count only in-flight mesh loads, not pending material polls.";
    EXPECT_NE(stepBody.find("!task.meshLoad ||"), std::string::npos)
        << "A mesh task that entered in-flight while the shared scene-load mesh cap was full has no meshLoad yet; polling must retry scheduling it instead of stranding it forever.";
    const auto startMeshLoad = actionsSource.find("bool StartMeshArtifactLoad");
    ASSERT_NE(startMeshLoad, std::string::npos);
    const auto startMeshLoadEnd = actionsSource.find(
        "std::optional<RendererResourceResolutionTask> PopNextRemainingTask",
        startMeshLoad);
    ASSERT_NE(startMeshLoadEnd, std::string::npos);
    const auto startMeshLoadBody = actionsSource.substr(startMeshLoad, startMeshLoadEnd - startMeshLoad);
    EXPECT_NE(startMeshLoadBody.find("maxSharedSceneLoadMeshLoads"), std::string::npos)
        << "Scene-load mesh scheduling needs a shared global cap distinct from the per-prefab in-flight window.";
    EXPECT_EQ(
        stepBody.find("state->inFlightTasks.size() >= state->streamingBudget.maxInflightMeshLoads"),
        std::string::npos)
        << "Pending material polls must not exhaust the mesh-load in-flight window.";
}

TEST(EditorLaunchArgsTests, SceneLoadRendererResolutionPrioritizesMeshBeforeFirstReveal)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto stepStart = actionsSource.find("void RunRendererResourceResolutionStep");
    ASSERT_NE(stepStart, std::string::npos);
    const auto stepEnd = actionsSource.find(
        "std::vector<NLS::Editor::Core::RendererResourceResolutionQueuePlanEntry>",
        stepStart);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepBody = actionsSource.substr(stepStart, stepEnd - stepStart);

    EXPECT_NE(
        actionsSource.find("RendererResourceResolutionTaskPopPreference::Mesh"),
        std::string::npos)
        << "Scene-load startup needs an explicit mesh-first queue mode for the blank-scene window.";
    EXPECT_NE(stepBody.find("state->shareSceneLoadFrameBudget"), std::string::npos);
    EXPECT_NE(stepBody.find("state->revealedObjectCount.load(std::memory_order_acquire) == 0u"), std::string::npos)
        << "The mesh-first preference should apply only until the first renderer-resource object is visible.";
    EXPECT_NE(stepBody.find("inFlightMeshTasks < state->streamingBudget.maxInflightMeshLoads"), std::string::npos)
        << "Mesh-first startup should still respect the scene-load mesh in-flight budget.";
    EXPECT_NE(stepBody.find("RendererResourceResolutionTaskPopPreference::Material"), std::string::npos)
        << "Once the mesh window is full, material work must continue instead of starving.";

    const auto popStart = actionsSource.find("std::optional<RendererResourceResolutionTask> PopNextRemainingTask");
    ASSERT_NE(popStart, std::string::npos);
    const auto popEnd = actionsSource.find("std::string RendererResourceResolutionSourceKey", popStart);
    ASSERT_NE(popEnd, std::string::npos);
    const auto popBody = actionsSource.substr(popStart, popEnd - popStart);
    const auto meshPreference = popBody.find("preference == RendererResourceResolutionTaskPopPreference::Mesh");
    ASSERT_NE(meshPreference, std::string::npos);
    const auto materialPreference = popBody.find("preference == RendererResourceResolutionTaskPopPreference::Material", meshPreference);
    ASSERT_NE(materialPreference, std::string::npos);
    const auto meshBranch = popBody.substr(meshPreference, materialPreference - meshPreference);
    EXPECT_EQ(meshBranch.find("return std::nullopt;"), std::string::npos)
        << "Mesh-first scene-load scheduling is only a preference; once a state has no remaining mesh tasks it must fall back to FIFO/material work instead of leaving remaining material tasks stuck forever.";
}

TEST(EditorLaunchArgsTests, SceneLoadTexturePumpChecksFrameBudgetDuringCompletions)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto stepStart = actionsSource.find("void RunRendererResourceResolutionStep");
    ASSERT_NE(stepStart, std::string::npos);
    const auto stepEnd = actionsSource.find(
        "std::vector<NLS::Editor::Core::RendererResourceResolutionQueuePlanEntry>",
        stepStart);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepBody = actionsSource.substr(stepStart, stepEnd - stepStart);

    const auto texturePump = stepBody.find("textureManager.PumpAsyncLoadsForPaths");
    ASSERT_NE(texturePump, std::string::npos);
    EXPECT_NE(stepBody.find("frameBudgetExpired", texturePump), std::string::npos)
        << "Scene-open texture completion pumping must yield after each heavy completion instead of waiting for the full batch.";

    const auto textureHeader = ReadTextFile("Runtime/Core/ResourceManagement/TextureManager.h");
    EXPECT_NE(textureHeader.find("std::function<bool()>"), std::string::npos)
        << "TextureManager should expose a per-completion stop predicate for frame-budgeted editor startup work.";
}

TEST(EditorLaunchArgsTests, SceneLoadTextureBindingUsesTextureManagerArtifactLookupIndex)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = actionsSource.find(
        "NLS::Core::ResourceManagement::TextureManager::Texture2D* FindCachedTextureByEquivalentPath");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = actionsSource.find(
        "NLS::Core::ResourceManagement::MeshManager::Mesh* FindCachedMeshByEquivalentPath",
        functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto functionBody = actionsSource.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(functionBody.find("textureManager.GetArtifactResource(path, false)"), std::string::npos)
        << "Material texture binding should use TextureManager's normalized artifact index instead of repeatedly scanning every cached texture.";
    EXPECT_EQ(functionBody.find("FindCachedResourceByEquivalentPath"), std::string::npos)
        << "Texture binding misses should not fall back to a per-slot full resource-table scan during scene-open reveal.";
}

TEST(EditorLaunchArgsTests, SceneLoadMaterialBindingUsesMaterialManagerArtifactLookupIndex)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = actionsSource.find(
        "NLS::Core::ResourceManagement::MaterialManager::Material* FindCachedMaterialByEquivalentPath");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = actionsSource.find(
        "NLS::Core::ResourceManagement::TextureManager::Texture2D* FindCachedTextureByEquivalentPath",
        functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto functionBody = actionsSource.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(functionBody.find("materialManager.FindRegisteredMaterialByEquivalentArtifactPath(path)"), std::string::npos)
        << "Scene-open material binding should use MaterialManager's normalized artifact index instead of scanning every cached material.";
    const auto indexedLookup = functionBody.find("materialManager.FindRegisteredMaterialByEquivalentArtifactPath(path)");
    const auto fallbackLookup = functionBody.find("FindCachedResourceByEquivalentPath");
    EXPECT_NE(fallbackLookup, std::string::npos)
        << "Material lookup still needs a correctness fallback when the single-entry artifact index is stale or overwritten.";
    EXPECT_LT(indexedLookup, fallbackLookup)
        << "The normalized material artifact index must stay on the hot path; the full resource-table scan is only a miss fallback.";
}

TEST(EditorLaunchArgsTests, SceneLoadMeshBindingDoesNotScanCachedMeshesWhileArtifactLoadIsPending)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = actionsSource.find("bool BindDeferredMeshPath");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = actionsSource.find("bool RunRendererResourceResolutionTask", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto functionBody = actionsSource.substr(functionStart, functionEnd - functionStart);

    const auto pendingLoadCheck = functionBody.find("if (!task.meshLoad)");
    ASSERT_NE(pendingLoadCheck, std::string::npos);
    const auto cachedMeshLookup = functionBody.find("FindCachedMeshByEquivalentPath");
    ASSERT_NE(cachedMeshLookup, std::string::npos);
    EXPECT_LT(pendingLoadCheck, cachedMeshLookup)
        << "In-flight mesh tasks should check whether the artifact load is still pending before doing a full cached-mesh equivalence scan.";
}

TEST(EditorLaunchArgsTests, SceneLoadRendererResolutionReportsImportProgressOncePerStep)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto stepStart = actionsSource.find("void RunRendererResourceResolutionStep");
    ASSERT_NE(stepStart, std::string::npos);
    const auto stepEnd = actionsSource.find(
        "std::vector<NLS::Editor::Core::RendererResourceResolutionQueuePlanEntry>",
        stepStart);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepBody = actionsSource.substr(stepStart, stepEnd - stepStart);

    size_t reportCalls = 0u;
    for (size_t pos = stepBody.find("reportTaskProgress();");
        pos != std::string::npos;
        pos = stepBody.find("reportTaskProgress();", pos + 1u))
    {
        ++reportCalls;
    }

    EXPECT_LE(reportCalls, 1u)
        << "Scene-open renderer resolution should publish progress once per step, not once per mesh/material task.";
}

TEST(EditorLaunchArgsTests, SceneLoadRendererResolutionTracesMainThreadSubsteps)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    EXPECT_NE(actionsSource.find("RendererResourceResolution::Diagnostics"), std::string::npos)
        << "Progress diagnostics must be trace-visible when they run on the editor thread.";
    EXPECT_NE(actionsSource.find("RendererResourceResolution::ReportProgress"), std::string::npos)
        << "Import progress publication must be trace-visible when diagnosing startup stalls.";
    EXPECT_NE(actionsSource.find("RendererResourceResolution::Task"), std::string::npos)
        << "Per-resource task execution must be trace-visible instead of appearing as anonymous Step time.";
    EXPECT_NE(actionsSource.find("RendererResourceResolution::BindMeshPath"), std::string::npos)
        << "Mesh binding must be trace-visible when diagnosing large startup mesh stalls.";
    EXPECT_NE(actionsSource.find("RendererResourceResolution::FindCachedMesh"), std::string::npos)
        << "Cached mesh lookup must be trace-visible because fallback equivalence scans can be expensive.";
    EXPECT_NE(actionsSource.find("RendererResourceResolution::CreateTransientMesh"), std::string::npos)
        << "Transient mesh creation must be trace-visible because it can synchronously allocate/upload buffers.";
    EXPECT_NE(actionsSource.find("RendererResourceResolution::BindMaterialPaths"), std::string::npos)
        << "Material binding must be trace-visible when diagnosing startup material stalls.";
    EXPECT_NE(actionsSource.find("RendererResourceResolution::BindMaterialTextures"), std::string::npos)
        << "Texture slot binding must be trace-visible when diagnosing startup material stalls.";
}

TEST(EditorLaunchArgsTests, SceneLoadRendererResolutionDiagnosticsAvoidBlockingMeshLoadLocks)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto logStart = actionsSource.find("void LogSceneLoadRendererResourceResolutionDiagnostics");
    ASSERT_NE(logStart, std::string::npos);
    const auto logEnd = actionsSource.find("size_t CountInFlightMeshRendererResourceTasks", logStart);
    ASSERT_NE(logEnd, std::string::npos);
    const auto logBody = actionsSource.substr(logStart, logEnd - logStart);
    EXPECT_NE(logBody.find("std::try_to_lock"), std::string::npos)
        << "Progress diagnostics must skip a sample instead of blocking startup frames on the resolution-state lifecycle lock.";
    EXPECT_EQ(logBody.find("std::lock_guard stateLock(state.lifecycleMutex)"), std::string::npos)
        << "Scene-load progress logging runs on the editor thread and cannot wait on lifecycleMutex.";
    EXPECT_NE(logBody.find("BuildRendererResourceResolutionProgressDiagnosticSnapshot"), std::string::npos)
        << "Progress diagnostics with visible task progress must use the cheap task-count snapshot instead of sampling async load managers on startup frames.";
    EXPECT_NE(logBody.find("lastSceneLoadDiagnosticCompletedTasks"), std::string::npos)
        << "Progress diagnostics should be skipped when neither task completion nor reveal count changed.";
    EXPECT_NE(logBody.find("std::chrono::seconds(5)"), std::string::npos)
        << "A stuck scene load should still emit an occasional progress diagnostic without sampling every frame.";
    EXPECT_NE(logBody.find("meshLoadDiagnostics=deferred"), std::string::npos)
        << "Progress logs should not print unsampled mesh-load counters as zero.";
    EXPECT_NE(logBody.find("asyncDiagnostics=deferred"), std::string::npos)
        << "Progress logs should make deferred global async diagnostics explicit.";
    EXPECT_NE(logBody.find("stalledProgressNeedsFullDiagnostics"), std::string::npos)
        << "A scene-load tail with no visible progress should occasionally sample mesh-load and global async manager state.";
    EXPECT_NE(logBody.find("needsFullDiagnostics"), std::string::npos)
        << "Only failed or stalled progress diagnostics should sample mesh-load and global async manager state on the editor thread.";
    EXPECT_NE(logBody.find("diagnosticSkipped=stateLockBusy"), std::string::npos)
        << "Terminal ready/failed diagnostics should leave an explicit breadcrumb if the lifecycle lock is busy.";
    const auto deferredMessage = logBody.find("meshLoadDiagnostics=deferred");
    ASSERT_NE(deferredMessage, std::string::npos);
    const auto progressMessageBranchStart = logBody.rfind("if (!needsFullDiagnostics)", deferredMessage);
    ASSERT_NE(progressMessageBranchStart, std::string::npos);
    const auto progressMessageBranchEnd = logBody.find("else", deferredMessage);
    ASSERT_NE(progressMessageBranchEnd, std::string::npos);
    const auto progressMessageBranch = logBody.substr(
        progressMessageBranchStart,
        progressMessageBranchEnd - progressMessageBranchStart);
    EXPECT_EQ(progressMessageBranch.find("meshLoadsPending="), std::string::npos)
        << "Cheap progress logs should not mix deferred diagnostics with unsampled mesh-load counters.";
    EXPECT_EQ(progressMessageBranch.find("globalMaterial"), std::string::npos)
        << "Cheap progress logs should not mix deferred diagnostics with unsampled global async counters.";
    EXPECT_EQ(progressMessageBranch.find("diagnosticSkipped=stateLockBusy"), std::string::npos)
        << "Progress lock misses should stay silent instead of logging every skipped startup-frame sample.";

    const auto fullDiagnosticsSelection = logBody.find("BuildRendererResourceResolutionDiagnosticSnapshot");
    ASSERT_NE(fullDiagnosticsSelection, std::string::npos);
    const auto fullDiagnosticsGuard = logBody.rfind("needsFullDiagnostics", fullDiagnosticsSelection);
    ASSERT_NE(fullDiagnosticsGuard, std::string::npos)
        << "Ready/normal progress logs should use the cheap snapshot; full diagnostics are reserved for failed or stalled restores.";
    const auto deferredMessageBranchStart = logBody.rfind("if (!needsFullDiagnostics)", deferredMessage);
    ASSERT_NE(deferredMessageBranchStart, std::string::npos)
        << "Ready/progress logs should explicitly mark mesh/global async diagnostics as deferred.";

    const auto lockMissBreadcrumb = logBody.find("diagnosticSkipped=stateLockBusy");
    ASSERT_NE(lockMissBreadcrumb, std::string::npos);
    const auto lockMissBlockStart = logBody.rfind("if (!isProgress)", lockMissBreadcrumb);
    ASSERT_NE(lockMissBlockStart, std::string::npos)
        << "Only terminal ready/failed diagnostics should log lifecycle-lock miss breadcrumbs.";

    const auto stepStart = actionsSource.find("void RunRendererResourceResolutionStep");
    ASSERT_NE(stepStart, std::string::npos);
    const auto stepEnd = actionsSource.find(
        "std::vector<NLS::Editor::Core::RendererResourceResolutionQueuePlanEntry>",
        stepStart);
    ASSERT_NE(stepEnd, std::string::npos);
    const auto stepBody = actionsSource.substr(stepStart, stepEnd - stepStart);
    const auto terminalMaterialFailure = stepBody.find("failedMaterialSlots > 0u");
    ASSERT_NE(terminalMaterialFailure, std::string::npos);
    const auto terminalFailureLog = stepBody.find(
        "LogSceneLoadRendererResourceResolutionDiagnostics(*state, \"failed\")",
        terminalMaterialFailure);
    ASSERT_NE(terminalFailureLog, std::string::npos)
        << "A terminal material-slot failure discovered after task drain should still emit full failed diagnostics.";

    const auto diagnosticsStart = actionsSource.find("RendererResourceResolutionDiagnosticSnapshot BuildRendererResourceResolutionDiagnosticSnapshot");
    ASSERT_NE(diagnosticsStart, std::string::npos);
    const auto diagnosticsEnd = actionsSource.find("void AppendAsyncArtifactRequestDiagnostics", diagnosticsStart);
    ASSERT_NE(diagnosticsEnd, std::string::npos);
    const auto diagnosticsBody = actionsSource.substr(diagnosticsStart, diagnosticsEnd - diagnosticsStart);

    EXPECT_NE(diagnosticsBody.find("std::try_to_lock"), std::string::npos)
        << "Progress diagnostics should not block the editor thread behind background mesh artifact completion locks.";
    EXPECT_EQ(diagnosticsBody.find("std::lock_guard loadsLock(state.asyncLoadsMutex)"), std::string::npos)
        << "Scene-load diagnostics must not wait for asyncLoadsMutex while background mesh jobs publish results.";
    EXPECT_NE(diagnosticsBody.find("meshLoadsSkipped"), std::string::npos)
        << "Sampled diagnostics should expose skipped mesh-load locks so logs are not mistaken for complete counts.";

    const auto progressDiagnosticsStart = actionsSource.find(
        "RendererResourceResolutionDiagnosticSnapshot BuildRendererResourceResolutionProgressDiagnosticSnapshot");
    ASSERT_NE(progressDiagnosticsStart, std::string::npos);
    const auto progressDiagnosticsEnd = actionsSource.find(
        "void AppendAsyncArtifactRequestDiagnostics",
        progressDiagnosticsStart);
    ASSERT_NE(progressDiagnosticsEnd, std::string::npos);
    const auto progressDiagnosticsBody = actionsSource.substr(
        progressDiagnosticsStart,
        progressDiagnosticsEnd - progressDiagnosticsStart);
    EXPECT_EQ(progressDiagnosticsBody.find("asyncLoadsMutex"), std::string::npos)
        << "Progress diagnostics run before the scene is visible and must not contend with mesh artifact workers.";
    EXPECT_EQ(progressDiagnosticsBody.find("GetAsyncArtifactRequestDiagnostics"), std::string::npos)
        << "Global async request diagnostics can take startup-visible locks; keep them for ready/failed logs.";
}

TEST(EditorLaunchArgsTests, SceneLoadProgressiveRevealCanShowMeshReadyObjectsWithDefaultMaterial)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto revealStart = actionsSource.find("void TryRevealRendererResourceResolutionObject");
    ASSERT_NE(revealStart, std::string::npos);
    const auto revealEnd = actionsSource.find("bool BuildPrefabAssetResolutionTasksFromTemplates", revealStart);
    ASSERT_NE(revealEnd, std::string::npos);
    const auto revealBody = actionsSource.substr(revealStart, revealEnd - revealStart);

    EXPECT_NE(revealBody.find("MeshFilterHasBoundMeshWithoutResolving(*meshFilter)"), std::string::npos)
        << "Scene-load progressive reveal should wait for mesh data before renderer unsuppression.";
    EXPECT_EQ(revealBody.find("HasResolvedMaterialBindings(*meshRenderer)"), std::string::npos)
        << "Scene View can render mesh-ready scene-load objects with its default material while material artifacts finish resolving.";
}

TEST(EditorLaunchArgsTests, SceneLoadRootVisibilityRestoreInvalidatesSceneRenderCache)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = actionsSource.find("void RestoreRendererResourceResolutionRootVisibility");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = actionsSource.find("void MarkRendererResourceResolutionSucceeded", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto functionBody = actionsSource.substr(functionStart, functionEnd - functionStart);

    const auto restoreRendering = functionBody.find("RestoreRendererResourceResolutionRootRendering(*root)");
    const auto markRenderChanged = functionBody.find("state.scene->MarkRenderContentChanged()");
    ASSERT_NE(restoreRendering, std::string::npos);
    ASSERT_NE(markRenderChanged, std::string::npos);
    EXPECT_LT(restoreRendering, markRenderChanged)
        << "Scene View static-frame caching must see a new render revision after a suppressed prefab root becomes visible.";
}

TEST(EditorLaunchArgsTests, SceneLoadRendererResourcesWaitForTextureTailBeforeStartupReady)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = actionsSource.find("bool BindDeferredMaterialPaths");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = actionsSource.find("bool BindDeferredMeshPath", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto functionBody = actionsSource.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(functionBody.find("const bool blockOnPendingTextures = !state.shareSceneLoadFrameBudget"), std::string::npos)
        << "Material task binding should still request texture slots without blocking each material task; the startup ready gate drains the tracked texture tail after all material paths have been discovered.";
    EXPECT_EQ(functionBody.find("const bool blockOnPendingTextures = true"), std::string::npos)
        << "Per-material texture waits can serialize large normal maps and delay discovery of later material texture slots.";

    const auto stepStart = actionsSource.find("void RunRendererResourceResolutionStep");
    ASSERT_NE(stepStart, std::string::npos);
    const auto readyLog = actionsSource.find("LogSceneLoadRendererResourceResolutionDiagnostics(*state, \"ready\")", stepStart);
    ASSERT_NE(readyLog, std::string::npos);
    const auto liveRootTextureBind = actionsSource.find("hasPendingTrackedRendererResourceTextures", stepStart);
    ASSERT_NE(liveRootTextureBind, std::string::npos);
    const auto pendingTextureGate = actionsSource.find("HasPendingTrackedRendererResourceTextureLoads(*state)", liveRootTextureBind);
    ASSERT_NE(pendingTextureGate, std::string::npos);
    const auto textureGateBody = actionsSource.substr(liveRootTextureBind, readyLog - liveRootTextureBind);

    EXPECT_EQ(textureGateBody.find("!state->shareSceneLoadFrameBudget"), std::string::npos)
        << "Startup scene readiness must wait until tracked material textures, including normal maps, are loaded and bound.";
    EXPECT_LT(pendingTextureGate, readyLog)
        << "The tracked texture tail must drain before reporting renderer resources ready.";
}

TEST(EditorLaunchArgsTests, SceneLoadMaterialBindingPrecedesTextureSlotResolution)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = actionsSource.find("bool BindDeferredMaterialPaths");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = actionsSource.find("bool BindDeferredMeshPath", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto functionBody = actionsSource.substr(functionStart, functionEnd - functionStart);

    const auto materialBind = functionBody.find("SetResolvedMaterialFromReference");
    const auto textureBind = functionBody.find("BindDeferredMaterialTextures");
    ASSERT_NE(materialBind, std::string::npos);
    ASSERT_NE(textureBind, std::string::npos);
    EXPECT_LT(materialBind, textureBind)
        << "Scene-open material tasks should bind the material before waiting on texture slots so Scene View can reveal geometry while textures stream in.";
}

TEST(EditorLaunchArgsTests, TextureSlotPendingWaitsKeepRendererResourceTasksIncomplete)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = actionsSource.find("bool BindDeferredMaterialTextures");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = actionsSource.find("bool BindDeferredMaterialPaths", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto functionBody = actionsSource.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(functionBody.find("blockOnPendingTextures"), std::string::npos)
        << "Texture slot waits must be explicit so renderer-resource readiness cannot accidentally ignore pending material textures.";
    EXPECT_NE(functionBody.find("IsAsyncArtifactLoadPending"), std::string::npos)
        << "Renderer restoration should be able to wait for pending texture artifacts.";
    EXPECT_NE(functionBody.find("return false"), std::string::npos)
        << "Blocking texture waits must keep material tasks incomplete until required texture slots resolve.";
}

TEST(EditorLaunchArgsTests, NonSceneLoadTextureFailuresKeepRendererResourceTaskFailed)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto functionStart = actionsSource.find("bool BindDeferredMaterialTextures");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = actionsSource.find("bool BindDeferredMaterialPaths", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto functionBody = actionsSource.substr(functionStart, functionEnd - functionStart);

    const auto failedTextureCheck = functionBody.find(
        "if (!texture && textureManager.IsAsyncArtifactLoadFailed(texturePath))");
    ASSERT_NE(failedTextureCheck, std::string::npos);
    const auto failedTextureBlockEnd = functionBody.find("if (!texture)", failedTextureCheck + 1u);
    ASSERT_NE(failedTextureBlockEnd, std::string::npos);
    const auto failedTextureBlock = functionBody.substr(
        failedTextureCheck,
        failedTextureBlockEnd - failedTextureCheck);
    EXPECT_NE(failedTextureBlock.find("blockOnPendingTextures"), std::string::npos)
        << "Scene-load reveal may tolerate missing textures, but non-scene-load deferred restoration must fail on texture load failure.";
    EXPECT_NE(failedTextureBlock.find("task.failed = true"), std::string::npos);
    EXPECT_NE(failedTextureBlock.find("state.failed = true"), std::string::npos);
    EXPECT_NE(failedTextureBlock.find("failedMaterialSlots"), std::string::npos);

    const auto missingTextureBlock = functionBody.substr(failedTextureBlockEnd);
    EXPECT_NE(missingTextureBlock.find("blockOnPendingTextures"), std::string::npos)
        << "A non-pending missing texture after an async request should fail non-scene-load resource restoration.";
    EXPECT_NE(missingTextureBlock.find("task.failed = true"), std::string::npos);
    EXPECT_NE(missingTextureBlock.find("state.failed = true"), std::string::npos);
    EXPECT_NE(missingTextureBlock.find("failedMaterialSlots"), std::string::npos);
}

TEST(EditorLaunchArgsTests, SceneLoadMaterialBindingRequestsTexturesWithoutBlockingReady)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto textureFunctionStart = actionsSource.find("bool BindDeferredMaterialTextures");
    ASSERT_NE(textureFunctionStart, std::string::npos);
    const auto textureFunctionEnd = actionsSource.find("bool BindDeferredMaterialPaths", textureFunctionStart);
    ASSERT_NE(textureFunctionEnd, std::string::npos);
    const auto textureFunctionBody = actionsSource.substr(textureFunctionStart, textureFunctionEnd - textureFunctionStart);
    EXPECT_NE(textureFunctionBody.find("requestMissingTextures"), std::string::npos)
        << "Texture binding should be able to bind cached textures without kicking off new async loads.";

    const auto materialFunctionStart = actionsSource.find("bool BindDeferredMaterialPaths");
    ASSERT_NE(materialFunctionStart, std::string::npos);
    const auto materialFunctionEnd = actionsSource.find("bool BindDeferredMeshPath", materialFunctionStart);
    ASSERT_NE(materialFunctionEnd, std::string::npos);
    const auto materialFunctionBody = actionsSource.substr(materialFunctionStart, materialFunctionEnd - materialFunctionStart);
    EXPECT_NE(materialFunctionBody.find("!state.shareSceneLoadFrameBudget"), std::string::npos)
        << "Scene-load material tasks should request missing textures without serially blocking each material slot.";
    const auto requestFlag = materialFunctionBody.find("const bool requestMissingTextures = true");
    const auto blockFlag = materialFunctionBody.find("const bool blockOnPendingTextures = !state.shareSceneLoadFrameBudget");
    const auto textureBindCall = materialFunctionBody.find("BindDeferredMaterialTextures");
    ASSERT_NE(requestFlag, std::string::npos);
    ASSERT_NE(blockFlag, std::string::npos);
    ASSERT_NE(textureBindCall, std::string::npos);
    EXPECT_LT(requestFlag, textureBindCall);
    EXPECT_LT(blockFlag, textureBindCall)
        << "Scene-load readiness must request material textures, while only non-scene-load restoration blocks on them.";

    const auto liveRootTextureBindStart = actionsSource.find("BindTrackedRendererResourceTexturesFromLiveRoot");
    ASSERT_NE(liveRootTextureBindStart, std::string::npos);
    const auto readyGateStart = actionsSource.find("HasPendingTrackedRendererResourceTextureLoads");
    ASSERT_NE(readyGateStart, std::string::npos);
    const auto stepStart = actionsSource.find("void RunRendererResourceResolutionStep");
    ASSERT_NE(stepStart, std::string::npos);
    const auto readyLog = actionsSource.find("LogSceneLoadRendererResourceResolutionDiagnostics(*state, \"ready\")", stepStart);
    ASSERT_NE(readyLog, std::string::npos);
    const auto liveRootTextureBind = actionsSource.find(
        "hasPendingTrackedRendererResourceTextures",
        stepStart);
    ASSERT_NE(liveRootTextureBind, std::string::npos)
        << "The live-root texture pass should bind ready textures without directly gating scene-load readiness.";
    ASSERT_NE(actionsSource.find("BindTrackedRendererResourceTexturesFromLiveRoot(*state", liveRootTextureBind), std::string::npos);
    const auto pendingGate = actionsSource.find("HasPendingTrackedRendererResourceTextureLoads(*state)", stepStart);
    ASSERT_NE(pendingGate, std::string::npos);
    const auto pendingGateCondition = actionsSource.find(
        "hasPendingTrackedRendererResourceTextures ||",
        liveRootTextureBind);
    ASSERT_NE(pendingGateCondition, std::string::npos)
        << "Startup readiness should drain already-requested live material textures before it opens the editor.";
    EXPECT_LT(liveRootTextureBind, pendingGateCondition)
        << "The ready gate must use the live-root texture pass result.";
    EXPECT_LT(liveRootTextureBind, readyLog)
        << "Renderer resource resolution should still bind any already-ready live material textures before reporting scene-load ready.";
    EXPECT_LT(pendingGateCondition, readyLog)
        << "The texture-tail gate must be evaluated before reporting ready.";
}

TEST(EditorLaunchArgsTests, SceneLoadRendererResourceReadyLogsNormalTextureReadiness)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");

    const auto statsStart = actionsSource.find("struct RendererResourceResolutionStats");
    ASSERT_NE(statsStart, std::string::npos);
    const auto statsEnd = actionsSource.find(
        "struct RendererResourceResolutionTextureReadinessSnapshot",
        statsStart);
    ASSERT_NE(statsEnd, std::string::npos);
    const auto statsBody = actionsSource.substr(statsStart, statsEnd - statsStart);
    EXPECT_EQ(statsBody.find("loadedTextureSlots"), std::string::npos);
    EXPECT_EQ(statsBody.find("normalTextureSlots"), std::string::npos);
    EXPECT_EQ(statsBody.find("loadedNormalTextureSlots"), std::string::npos)
        << "Cumulative texture counters can exceed total live slots after rebinding and must not be retained as evidence.";

    const auto snapshotEnd = actionsSource.find(
        "std::mutex g_rendererResourceResolutionStatesMutex",
        statsEnd);
    ASSERT_NE(snapshotEnd, std::string::npos);
    const auto snapshotBody = actionsSource.substr(statsEnd, snapshotEnd - statsEnd);
    EXPECT_NE(snapshotBody.find("normalTextureSlots"), std::string::npos);
    EXPECT_NE(snapshotBody.find("loadedNormalTextureSlots"), std::string::npos)
        << "Ready diagnostics must use a terminal live-root snapshot.";

    const auto textureFunctionStart = actionsSource.find("bool BindDeferredMaterialTextures");
    ASSERT_NE(textureFunctionStart, std::string::npos);
    const auto textureFunctionEnd = actionsSource.find("bool BindDeferredMaterialPaths", textureFunctionStart);
    ASSERT_NE(textureFunctionEnd, std::string::npos);
    const auto textureFunctionBody = actionsSource.substr(textureFunctionStart, textureFunctionEnd - textureFunctionStart);
    EXPECT_NE(textureFunctionBody.find("BindRendererResourceMaterialTextureParameter"), std::string::npos)
        << "Imported ShaderLab material texture slots can be metadata-only on the source shader; "
           "startup binding must still store the loaded Texture2D* so deferred GBuffer normal mapping can sync it.";

    const auto readyLogStart = actionsSource.find("const auto textureReadiness = BuildLiveRendererResourceTextureReadinessSnapshot");
    ASSERT_NE(readyLogStart, std::string::npos);
    const auto readyLogEnd = actionsSource.find("finishFailed", readyLogStart);
    ASSERT_NE(readyLogEnd, std::string::npos);
    const auto readyLogBody = actionsSource.substr(readyLogStart, readyLogEnd - readyLogStart);
    EXPECT_NE(readyLogBody.find("BuildLiveRendererResourceTextureReadinessSnapshot"), std::string::npos)
        << "The startup-ready log must rescan the live scene so normal-map readiness cannot exceed the slot total.";
    EXPECT_NE(readyLogBody.find("normalTextureSlots="), std::string::npos)
        << "Before/after startup evidence needs a direct normal-map readiness count.";
    EXPECT_EQ(readyLogBody.find("state->stats->loadedNormalTextureSlots"), std::string::npos)
        << "Cumulative loaded-normal counters double-count live-root rebinding and are not valid startup-ready evidence.";
}

TEST(EditorLaunchArgsTests, SceneViewSuppressesVisibleTextureRequestsWhileSceneLoadResourcesArePending)
{
    const auto rendererHeader = ReadTextFile("Runtime/Engine/Rendering/BaseSceneRenderer.h");
    EXPECT_NE(rendererHeader.find("suppressVisibleMaterialTextureRequests"), std::string::npos);
    EXPECT_NE(rendererHeader.find("allowDefaultMaterialForUnresolvedExplicitMaterials"), std::string::npos);

    const auto rendererSource = ReadTextFile("Runtime/Engine/Rendering/BaseSceneRenderer.cpp");
    const auto pumpStart = rendererSource.find("BaseSceneRenderer::ParseScene::PumpVisibleMaterialTextures");
    ASSERT_NE(pumpStart, std::string::npos);
    const auto budgetedPump = rendererSource.find("texturePumpBudget", pumpStart);
    ASSERT_NE(budgetedPump, std::string::npos);
    const auto pumpGuardStart = rendererSource.rfind("suppressVisibleMaterialTextureRequests", pumpStart);
    ASSERT_NE(pumpGuardStart, std::string::npos)
        << "Scene-load startup should not let visible material texture requests starve mesh/material resource restoration.";

    const auto sceneViewSource = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto descriptorStart = sceneViewSource.find("SceneView::CreateSceneDescriptor");
    ASSERT_NE(descriptorStart, std::string::npos);
    const auto descriptorEnd = sceneViewSource.find("return descriptor;", descriptorStart);
    ASSERT_NE(descriptorEnd, std::string::npos);
    const auto descriptorBody = sceneViewSource.substr(descriptorStart, descriptorEnd - descriptorStart);
    EXPECT_NE(
        descriptorBody.find("descriptor.suppressVisibleMaterialTextureRequests"),
        std::string::npos);
    EXPECT_NE(descriptorBody.find("descriptor.suppressHZBOcclusion"), std::string::npos);
    EXPECT_NE(
        descriptorBody.find("descriptor.allowDefaultMaterialForUnresolvedExplicitMaterials"),
        std::string::npos);
    EXPECT_NE(descriptorBody.find("pendingSceneLoadResourceTasks > 0u"), std::string::npos);

    EXPECT_NE(rendererSource.find("sceneDescriptor.suppressHZBOcclusion"), std::string::npos)
        << "Scene-load startup should not build HZB frame resources while renderer resources are still being restored.";
    EXPECT_NE(
        rendererSource.find("sceneDescriptor.allowDefaultMaterialForUnresolvedExplicitMaterials"),
        std::string::npos)
        << "Scene-load startup can tolerate unresolved explicit materials in the renderer so mesh-ready objects can appear with a default material.";

    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    EXPECT_NE(actionsSource.find("revealed="), std::string::npos)
        << "Scene-load diagnostics should report how many renderer-resource objects have become visible.";
    EXPECT_NE(actionsSource.find("options.allowProgressiveRevealBeforeAllResourcesReady = true"), std::string::npos)
        << "Scene-load startup should allow mesh-ready objects to appear before the material tail drains.";
    EXPECT_EQ(actionsSource.find("if (!HasResolvedMaterialBindings(*meshRenderer))"), std::string::npos)
        << "Scene-load progressive reveal should not keep Scene View empty while waiting for explicit material artifacts.";
}

TEST(EditorLaunchArgsTests, VisibleMaterialTexturePumpIsFrameBudgeted)
{
    const auto rendererHeader = ReadTextFile("Runtime/Engine/Rendering/BaseSceneRenderer.h");
    EXPECT_EQ(rendererHeader.find("m_opaqueVisibleMaterialTexturePumpCursor"), std::string::npos)
        << "Visible texture pump cursors should not change BaseSceneRenderer object layout.";

    const auto rendererSource = ReadTextFile("Runtime/Engine/Rendering/BaseSceneRenderer.cpp");
    EXPECT_NE(rendererSource.find("VisibleMaterialTexturePumpState"), std::string::npos);
    EXPECT_NE(rendererSource.find("VisibleMaterialTexturePumpStepResult"), std::string::npos);
    EXPECT_NE(rendererSource.find("completedFullScan"), std::string::npos)
        << "Readback must distinguish a fully scanned visible material set from a budget-expired slice.";
    EXPECT_NE(rendererSource.find("ScannedSinceLastRequest"), std::string::npos)
        << "Large scenes need visible texture scan completion to accumulate across frame-budgeted slices.";
    EXPECT_NE(rendererHeader.find("HasCompletedVisibleMaterialTexturePumpForReadback"), std::string::npos)
        << "Scene validation readback needs a renderer-level signal that visible material textures have been pumped.";
    EXPECT_NE(rendererSource.find("ForgetVisibleMaterialTexturePumpState(*this)"), std::string::npos)
        << "The ABI-safe side table must be cleaned when a scene renderer is destroyed.";
    const auto helperStart = rendererSource.find("VisibleMaterialTexturePumpStepResult PumpOneVisibleMaterialTexture");
    ASSERT_NE(helperStart, std::string::npos);
    const auto helperEnd = rendererSource.find("void HashCombine", helperStart);
    ASSERT_NE(helperEnd, std::string::npos);
    const auto helperBody = rendererSource.substr(helperStart, helperEnd - helperStart);
    EXPECT_NE(helperBody.find("VisibleMaterialTexturePumpBudget& budget"), std::string::npos);
    EXPECT_NE(helperBody.find("cursor"), std::string::npos)
        << "Visible texture pumping should rotate through large scenes instead of rescanning from the first material every frame.";
    EXPECT_NE(helperBody.find("VisibleMaterialTexturePumpBudgetExpired"), std::string::npos)
        << "Visible texture pumping must stop after the per-frame slice is exhausted.";

    const auto readbackGateStart = rendererSource.find("HasCompletedVisibleMaterialTexturePumpForReadback");
    ASSERT_NE(readbackGateStart, std::string::npos);
    const auto readbackGateEnd = rendererSource.find("#if defined(NLS_ENABLE_TEST_HOOKS)", readbackGateStart);
    ASSERT_NE(readbackGateEnd, std::string::npos);
    const auto readbackGateBody = rendererSource.substr(readbackGateStart, readbackGateEnd - readbackGateStart);
    EXPECT_NE(readbackGateBody.find("lastPumpCompletedFullScan"), std::string::npos);
    EXPECT_NE(readbackGateBody.find("GetAsyncArtifactRequestDiagnostics"), std::string::npos)
        << "Scene readback should wait for queued visible texture artifact loads to be consumed.";

    const auto pumpStart = rendererSource.find("BaseSceneRenderer::ParseScene::PumpVisibleMaterialTextures");
    ASSERT_NE(pumpStart, std::string::npos);
    const auto pumpEnd = rendererSource.find("logStartupParseSceneStage(\"PumpVisibleMaterialTextures\")", pumpStart);
    ASSERT_NE(pumpEnd, std::string::npos);
    const auto pumpBody = rendererSource.substr(pumpStart, pumpEnd - pumpStart);
    EXPECT_NE(pumpBody.find("std::chrono::milliseconds(2)"), std::string::npos)
        << "Ready-after-scene-load texture requests should not monopolize the first visible frame.";
    EXPECT_NE(pumpBody.find("256u"), std::string::npos)
        << "A material-count cap keeps pending texture scans bounded even when individual requests are cheap.";
    EXPECT_NE(pumpBody.find("texturePumpBudget"), std::string::npos)
        << "Opaque, decal, and transparent queues should share one per-frame texture pump budget.";
    EXPECT_NE(pumpBody.find("lastPumpCompletedFullScan"), std::string::npos)
        << "The pump should publish whether this frame completed a full visible material texture scan.";
    EXPECT_NE(pumpBody.find("while (!VisibleMaterialTexturePumpBudgetExpired(texturePumpBudget))"), std::string::npos)
        << "Visible texture pumping should issue more than one missing texture request per frame when the budget allows; "
           "otherwise fully textured large scenes take one frame per missing texture to settle.";
    EXPECT_EQ(pumpBody.find("if (!opaquePump.requestedTexture)"), std::string::npos)
        << "A single opaque texture request must not stop decal/transparent queues or the rest of the visible-material pump.";

    const auto sceneViewSource = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    EXPECT_EQ(sceneViewSource.find("HasCompletedVisibleMaterialTexturePumpForReadback"), std::string::npos)
        << "Scene validation readback should rely on scene-load renderer resource readiness, not a second visible-material full scan.";
    EXPECT_EQ(sceneViewSource.find("kSceneLoadValidationVisibleTexturePumpMaxWaitFrames"), std::string::npos)
        << "Scene validation readback should not spend a fixed frame cap waiting for the visible texture pump after scene-load reveal.";
}

TEST(EditorLaunchArgsTests, SceneLoadRendererResourceManagersKeepEnoughAsyncArtifactWorkersActive)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async artifact caps.";
#else
    EXPECT_EQ(
        NLS::Core::ResourceManagement::MeshManager::GetMaxPendingAsyncArtifactRequestCountForTesting(),
        16u)
        << "Scene-open generated-prefab restore queues hundreds of mesh artifact reads, but over-saturating mesh IO can delay material resolution during full scene reveal.";
    EXPECT_EQ(
        NLS::Core::ResourceManagement::MaterialManager::GetMaxPendingAsyncArtifactRequestCountForTesting(),
        32u)
        << "Scene-open generated-prefab restore queues hundreds of material references; a 16-worker cap leaves material artifacts queued during startup.";
    EXPECT_EQ(
        NLS::Core::ResourceManagement::TextureManager::GetMaxPendingAsyncArtifactRequestCountForTesting(),
        32u)
        << "Scene-open material readiness depends on texture artifact progress; a 16-worker cap leaves dozens of texture artifacts queued during startup.";
#endif
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
