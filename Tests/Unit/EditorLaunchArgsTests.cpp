#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
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
#include "Threading/ExecutionThreadBudget.h"

namespace
{
#if defined(_WIN32)
    class ScopedEnvironmentVariable final
    {
    public:
        ScopedEnvironmentVariable(const char* name, const char* value) : m_name(name)
        {
            const char* previous = std::getenv(name);
            m_previous = previous != nullptr ? previous : "";
            _putenv_s(name, value);
        }

        ~ScopedEnvironmentVariable() { _putenv_s(m_name.c_str(), m_previous.c_str()); }

        ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
        ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    private:
        std::string m_name;
        std::string m_previous;
    };
#endif

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

TEST(EditorLaunchArgsTests, ParsesAssetPrewarmAndStartupResourcePipeline)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-prewarm-assets",
        "--editor-startup-resource-pipeline",
        "legacy",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.prewarmAssetsOnly);
    EXPECT_EQ(
        parsed.startupResourcePipeline,
        NLS::Editor::Launch::StartupResourcePipeline::Legacy);
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, Dx12DebugLayerIsOptInForEditor)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);
    const auto defaultArgs = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);
    EXPECT_FALSE(defaultArgs.hasError);
    EXPECT_FALSE(defaultArgs.dx12DebugLayer);

    argv = MutableArgv({"Editor.exe", "--editor-dx12-debug-layer", "TestProject.nullus"}, storage);
    const auto validationArgs = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);
    EXPECT_FALSE(validationArgs.hasError);
    EXPECT_TRUE(validationArgs.dx12DebugLayer);
    EXPECT_EQ(validationArgs.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, DefaultStartupResourcePipelineFollowsBuildConfiguration)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({"Editor.exe", "TestProject.nullus"}, storage);
    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_EQ(parsed.startupResourcePipeline, NLS::Editor::Launch::DefaultStartupResourcePipeline());
#if defined(_WIN32)
    EXPECT_EQ(parsed.startupResourcePipeline, NLS::Editor::Launch::StartupResourcePipeline::Pipelined);
#else
    EXPECT_EQ(parsed.startupResourcePipeline, NLS::Editor::Launch::StartupResourcePipeline::Async);
#endif
}

TEST(EditorLaunchArgsTests, ExplicitAsyncOverridesDebugStartupResourcePipelineDefault)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-startup-resource-pipeline",
        "async",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_EQ(parsed.startupResourcePipeline, NLS::Editor::Launch::StartupResourcePipeline::Async);
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, ParsesPipelinedStartupResourcePipeline)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe", "--editor-startup-resource-pipeline", "pipelined", "TestProject.nullus"
    }, storage);
    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);
    EXPECT_FALSE(parsed.hasError);
    EXPECT_EQ(parsed.startupResourcePipeline, NLS::Editor::Launch::StartupResourcePipeline::Pipelined);
}

TEST(EditorLaunchArgsTests, RejectsInvalidStartupResourcePipeline)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-startup-resource-pipeline",
        "invalid",
        "TestProject.nullus"
    }, storage);

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

TEST(EditorLaunchArgsTests, ParsesEditorValidationCameraForwardFrames)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-validation-camera-forward-frames",
        "180",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.hasDiagnosticsOverride);
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationCameraForwardFrames, 180u);
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, ParsesEditorCameraPerformanceBenchmark)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-camera-performance-output",
        "D:/perf/debug.json",
        "--editor-camera-performance-warmup-frames",
        "30",
        "--editor-camera-performance-frames",
        "300",
        "--editor-camera-performance-settle-frames",
        "120",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_FALSE(parsed.hasError);
    EXPECT_TRUE(parsed.hasDiagnosticsOverride);
    EXPECT_EQ(parsed.diagnosticsSettings.editorCameraPerformanceOutput, "D:/perf/debug.json");
    EXPECT_EQ(parsed.diagnosticsSettings.editorCameraPerformanceWarmupFrames, 30u);
    EXPECT_EQ(parsed.diagnosticsSettings.editorCameraPerformanceFrames, 300u);
    EXPECT_EQ(parsed.diagnosticsSettings.editorCameraPerformanceSettleFrames, 120u);
    EXPECT_EQ(parsed.diagnosticsSettings.editorValidationCameraForwardFrames, 330u);
    EXPECT_EQ(parsed.projectPathArgument, "TestProject.nullus");
}

TEST(EditorLaunchArgsTests, RejectsZeroEditorCameraPerformanceFrameCounts)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-camera-performance-output",
        "D:/perf/debug.json",
        "--editor-camera-performance-frames",
        "0",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_TRUE(parsed.hasError);
}

TEST(EditorLaunchArgsTests, RejectsEditorCameraPerformanceWithProfilerOrTimelineTrace)
{
    std::vector<std::string> profilerStorage;
    char** profilerArgv = MutableArgv({
        "Editor.exe",
        "--editor-camera-performance-output",
        "D:/perf/profiler.json",
        "--editor-validation-open-profiler",
        "TestProject.nullus"
    }, profilerStorage);
    EXPECT_TRUE(NLS::Editor::Launch::ParseEditorArgs(
        static_cast<int>(profilerStorage.size()),
        profilerArgv).hasError);

    std::vector<std::string> traceStorage;
    char** traceArgv = MutableArgv({
        "Editor.exe",
        "--editor-validation-trace-frames",
        "300",
        "--editor-camera-performance-output",
        "D:/perf/trace.json",
        "TestProject.nullus"
    }, traceStorage);
    EXPECT_TRUE(NLS::Editor::Launch::ParseEditorArgs(
        static_cast<int>(traceStorage.size()),
        traceArgv).hasError);
}

TEST(EditorLaunchArgsTests, RejectsEditorCameraPerformanceFrameCountOverflow)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-camera-performance-output",
        "D:/perf/debug.json",
        "--editor-camera-performance-warmup-frames",
        "4294967295",
        "--editor-camera-performance-frames",
        "1",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_TRUE(parsed.hasError);
}

TEST(EditorLaunchArgsTests, RejectsEditorCameraPerformanceSettleFrameCountOverflow)
{
    std::vector<std::string> storage;
    char** argv = MutableArgv({
        "Editor.exe",
        "--editor-camera-performance-output",
        "D:/perf/debug.json",
        "--editor-camera-performance-settle-frames",
        "4294967296",
        "TestProject.nullus"
    }, storage);

    const auto parsed = NLS::Editor::Launch::ParseEditorArgs(static_cast<int>(storage.size()), argv);

    EXPECT_TRUE(parsed.hasError);
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
        {8u, 2u, 1u},
        {12u, 4u, 1u},
        {16u, 5u, 2u},
        {20u, 6u, 3u},
        {64u, 13u, 6u}
    };

    for (const auto& testCase : cases)
    {
        const auto budget = NLS::Editor::Core::ResolveEditorJobWorkerBudget(testCase.hardwareConcurrency);
        EXPECT_EQ(budget.foregroundWorkerCount, testCase.expectedForeground);
        EXPECT_EQ(budget.backgroundWorkerCount, testCase.expectedBackground);
        const auto poolBudget = NLS::Base::Threading::ResolveExecutionPoolBudget(
            testCase.hardwareConcurrency);
        // The rendering TaskGraph and the Jobs pools must fit inside the same budget.
        EXPECT_EQ(budget.foregroundWorkerCount, poolBudget.jobsForegroundWorkerCount);
        EXPECT_EQ(budget.backgroundWorkerCount, poolBudget.jobsBackgroundWorkerCount);
        EXPECT_EQ(
            poolBudget.taskGraphWorkerCount +
                budget.foregroundWorkerCount +
                budget.backgroundWorkerCount,
            poolBudget.totalPoolWorkerCount);
        EXPECT_EQ(
            budget.foregroundWorkerCount + budget.backgroundWorkerCount,
            poolBudget.jobsWorkerCount);
    }
}

TEST(EditorLaunchArgsTests, EditorJobWorkerOverrideKeepsTheSameTotalBudget)
{
    const auto twoWorkers = NLS::Editor::Core::ResolveEditorJobWorkerBudget(20u, 2u);
    EXPECT_EQ(twoWorkers.foregroundWorkerCount, 7u);
    EXPECT_EQ(twoWorkers.backgroundWorkerCount, 2u);

    const auto eightWorkers = NLS::Editor::Core::ResolveEditorJobWorkerBudget(20u, 8u);
    EXPECT_EQ(eightWorkers.foregroundWorkerCount, 1u);
    EXPECT_EQ(eightWorkers.backgroundWorkerCount, 8u);

    const auto sixteenWorkers = NLS::Editor::Core::ResolveEditorJobWorkerBudget(20u, 16u);
    EXPECT_EQ(sixteenWorkers.foregroundWorkerCount, 1u);
    EXPECT_EQ(sixteenWorkers.backgroundWorkerCount, 8u);

    const auto clampedWorkers = NLS::Editor::Core::ResolveEditorJobWorkerBudget(64u, 64u);
    EXPECT_EQ(clampedWorkers.backgroundWorkerCount, 16u);
    EXPECT_EQ(clampedWorkers.foregroundWorkerCount, 3u);
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

TEST(EditorLaunchArgsTests, RendererResourceTailReusesValidSceneTextureWithoutManagerOwnership)
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
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter), oldTexture.get());

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

TEST(EditorLaunchArgsTests, EditorWritesThumbnailTelemetrySummaryOnShutdownWhenConfigured)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Core/Editor.cpp");

    EXPECT_NE(source.find("WriteThumbnailTelemetrySummaryIfRequested"), std::string::npos)
        << "Asset Browser thumbnail telemetry needs an editor-visible summary export path so "
           "performance work can use real capture data instead of guessing.";
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

TEST(EditorLaunchArgsTests, SceneViewStaticFrameCacheFollowsEditorSetting)
{
    const auto source = ReadTextFile("Project/Editor/Panels/SceneView.cpp");
    const auto functionStart = source.find("bool Editor::Panels::SceneView::ShouldUseStaticFrameCache() const");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("uint64_t Editor::Panels::SceneView::BuildStaticFrameCacheKey", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(
        body.find("GetRenderingSettingsObject().enableSceneViewStaticFrameCache"),
        std::string::npos)
        << "Scene View static caching must follow the editor setting instead of being unconditionally enabled.";
    EXPECT_EQ(body.find("return true;"), std::string::npos)
        << "A hard-coded true bypasses the default-off profiling setting.";
}

TEST(EditorLaunchArgsTests, SceneRestoreGeneratedModelPrefabsAreNotMarkedMissingBeforeDeferredResolution)
{
    const auto actionsSource = ReadTextFile("Project/Editor/Core/EditorActions.cpp");
    const auto restoreStart = actionsSource.find("bool Editor::Core::EditorActions::RestorePrefabInstancesForCurrentSceneFromDisk(");
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

TEST(EditorLaunchArgsTests, SceneRestoreGeneratedModelPrefabInstantiationPrewarmsRuntimeResources)
{
    const auto facadeSource = ReadTextFile("Project/Editor/Assets/PrefabUtilityFacade.cpp");
    const auto instantiateStart = facadeSource.find("PrefabOperationResult InstantiateStrippedPrefabInstance(");
    ASSERT_NE(instantiateStart, std::string::npos);
    const auto instantiateEnd = facadeSource.find(
        "PrefabOperationResult RestoreEngineStylePrefabInstancesFromSceneDocument(",
        instantiateStart);
    ASSERT_NE(instantiateEnd, std::string::npos);
    const auto instantiateBody = facadeSource.substr(instantiateStart, instantiateEnd - instantiateStart);

    EXPECT_NE(
        instantiateBody.find(
            "const bool deferGeneratedModelResources = sourcePrefab->generatedModelPrefab;"),
        std::string::npos);
    EXPECT_NE(
        instantiateBody.find("request.deferAssetReferenceResolution = deferGeneratedModelResources;"),
        std::string::npos);
    EXPECT_NE(
        instantiateBody.find(
            "request.synchronousAssetReferencePrewarm = synchronousAssetReferencePrewarm;"),
        std::string::npos)
        << "Generated scene prefabs must honor the selected startup resource pipeline.";
    EXPECT_NE(
        instantiateBody.find("request.skipDeferredAssetReferenceCacheLookup = deferGeneratedModelResources;"),
        std::string::npos);
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

TEST(EditorLaunchArgsTests, InspectorPrefabOverrideMarkersUseCachedPresentationStates)
{
    const auto inspectorSource = ReadTextFile("Project/Editor/Inspector/InspectorView.cpp");
    EXPECT_EQ(
        inspectorSource.find("GetPrefabOverrideState("),
        std::string::npos)
        << "Inspector draw paths must read prefab markers from the presentation refreshed once per dirty revision instead of re-discovering the full prefab graph per control.";
    EXPECT_NE(
        inspectorSource.find("m_prefabOverrideQuery->GetState"),
        std::string::npos)
        << "Inspector controls must share the revision-scoped prefab override query instead of re-discovering the prefab graph.";

    const auto prefabInspectorSource = ReadTextFile("Project/Editor/Inspector/PrefabInspectorSupport.cpp");
    const auto refreshStart = prefabInspectorSource.find("void RefreshPrefabOverrideStates");
    ASSERT_NE(refreshStart, std::string::npos);
    const auto refreshEnd = prefabInspectorSource.find("PrefabOverrideState GetPrefabOverrideState", refreshStart);
    ASSERT_NE(refreshEnd, std::string::npos);
    const auto refreshBody = prefabInspectorSource.substr(refreshStart, refreshEnd - refreshStart);
    EXPECT_NE(refreshBody.find("tree.auxiliary"), std::string::npos)
        << "GameObject header rows live in the presentation auxiliary tree and must share the same prefab snapshot cache.";

    EXPECT_NE(
        prefabInspectorSource.find("GetPrefabOverrideMarkersForObject"),
        std::string::npos)
        << "Inspector markers must use marker-only discovery instead of materializing override base/local values.";

    const auto nativeAdapterSource = ReadTextFile("Project/Editor/Inspector/NativeReflectionAdapter.cpp");
    EXPECT_EQ(
        nativeAdapterSource.find("RefreshPrefabOverrideStates("),
        std::string::npos)
        << "Native presentation should build chrome only; InspectorView owns the single dirty-revision prefab refresh.";
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

TEST(EditorLaunchArgsTests, EditorFrameRingCoversConcurrentViewsOnTopOfFramesInFlight)
{
    // The Editor is not a single-view application: one tick can submit a Scene View
    // frame, a Game View frame, UI-only frames and several thumbnail frames, and each
    // one opens a slot. The ring therefore carries a headroom on top of the pipelining
    // depth instead of the single extra slot a single-view application needs.
    EXPECT_EQ(NLS::Editor::Core::ResolveEditorFrameResourceCount(0u), 1u);
    EXPECT_GT(NLS::Editor::Core::kEditorConcurrentViewFrameHeadroom, 1u)
        << "a single extra slot is the single-view sizing this headroom exists to replace";

    for (const uint32_t framesInFlight : {1u, 2u, 3u, 5u})
    {
        EXPECT_EQ(
            NLS::Editor::Core::ResolveEditorFrameResourceCount(framesInFlight),
            framesInFlight + NLS::Editor::Core::kEditorConcurrentViewFrameHeadroom);
    }
}

#if defined(_WIN32)
TEST(EditorLaunchArgsTests, DefaultDx12DeviceCreationKeepsPageFaultDiagnosticsWithoutAutoBreadcrumbs)
{
    const ScopedEnvironmentVariable debugLayer("NLS_DX12_DEBUG_LAYER", "0");
    const ScopedEnvironmentVariable gpuValidation("NLS_DX12_GPU_VALIDATION", "0");
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
    EXPECT_FALSE(resources.dredAutoBreadcrumbsEnabled);
}

TEST(EditorLaunchArgsTests, ExplicitDebugValidationDx12DeviceCreationEnablesDredDiagnostics)
{
    const ScopedEnvironmentVariable debugLayer("NLS_DX12_DEBUG_LAYER", "0");
    const ScopedEnvironmentVariable gpuValidation("NLS_DX12_GPU_VALIDATION", "0");
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
    EXPECT_TRUE(resources.dredAutoBreadcrumbsEnabled);
}

TEST(EditorLaunchArgsTests, DisabledDebugValidationDx12DeviceCreationDisablesAutoBreadcrumbs)
{
    const ScopedEnvironmentVariable debugLayer("NLS_DX12_DEBUG_LAYER", "0");
    const ScopedEnvironmentVariable gpuValidation("NLS_DX12_GPU_VALIDATION", "0");
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
    EXPECT_FALSE(resources.dredAutoBreadcrumbsEnabled);
}

TEST(EditorLaunchArgsTests, EnvironmentDebugLayerEnablesBreadcrumbsWithDebugModeDisabled)
{
    const ScopedEnvironmentVariable debugLayer("NLS_DX12_DEBUG_LAYER", "1");
    const ScopedEnvironmentVariable gpuValidation("NLS_DX12_GPU_VALIDATION", "0");
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable or Shader Model 6 unsupported on this test machine";
    ASSERT_TRUE(resources.IsValid()) << resources.creationDiagnostics;
    EXPECT_TRUE(resources.dredDiagnosticsEnabled);
    EXPECT_TRUE(resources.dredAutoBreadcrumbsEnabled);
}

TEST(EditorLaunchArgsTests, EnvironmentGpuValidationEnablesBreadcrumbsWithDebugModeDisabled)
{
    const ScopedEnvironmentVariable debugLayer("NLS_DX12_DEBUG_LAYER", "0");
    const ScopedEnvironmentVariable gpuValidation("NLS_DX12_GPU_VALIDATION", "1");
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable or Shader Model 6 unsupported on this test machine";
    ASSERT_TRUE(resources.IsValid()) << resources.creationDiagnostics;
    EXPECT_TRUE(resources.dredDiagnosticsEnabled);
    EXPECT_TRUE(resources.dredAutoBreadcrumbsEnabled);
}
#endif
