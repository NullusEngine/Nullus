#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
// Heap validation used by the scene-mutation run to localise corruption.
#include <crtdbg.h>
#endif

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/LightComponent.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/RenderCommandPipe.h"
#include "Rendering/Context/RenderCommandFence.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/Context/FrameSubmissionTelemetry.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/ForwardSceneRenderer.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h"
#include "Rendering/Settings/DriverSettings.h"
#include "SceneSystem/Scene.h"
#if defined(_WIN32) || defined(__APPLE__)
#include "Windowing/Context/Device.h"
#include "Windowing/Settings/WindowSettings.h"
#include "Windowing/Window.h"
#endif

namespace
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    class ScopedVulkanValidationCheck final
    {
    public:
        ~ScopedVulkanValidationCheck()
        {
            EXPECT_EQ(NLS::Render::Backend::GetVulkanValidationErrorCountForTesting(), m_before)
                << "Vulkan validation reported errors; inspect stderr as well as stdout.";
        }
    private:
        uint64_t m_before = NLS::Render::Backend::GetVulkanValidationErrorCountForTesting();
    };
#endif

    /**
     * Registers the driver for the duration of one test.
     *
     * The service locator hands out a non-owning reference, so a driver that is a
     * test local must be unregistered before it dies; otherwise every later test
     * in the process inherits a dangling service.
     */
    class ScopedDriverService final
    {
    public:
        explicit ScopedDriverService(NLS::Render::Context::Driver& driver)
        {
            NLS::Core::ServiceLocator::Provide(driver);
        }

        ~ScopedDriverService()
        {
            NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
        }

        ScopedDriverService(const ScopedDriverService&) = delete;
        ScopedDriverService& operator=(const ScopedDriverService&) = delete;
    };

    class ScopedFrameDrain final
    {
    public:
        explicit ScopedFrameDrain(NLS::Render::Context::Driver& driver) : m_driver(driver) {}
        ~ScopedFrameDrain()
        {
            (void)NLS::Render::Context::DriverRendererAccess::TryWaitForSubmittedGpuWork(m_driver);
        }
    private:
        NLS::Render::Context::Driver& m_driver;
    };

    struct RhiFrameObservation
    {
        uint64_t frameId = 0u;
        bool offscreenOnly = false;
        bool usedExternalOutputBridge = false;
        bool submittedSuccessfully = false;
        bool droppedForSwapchainRecreation = false;
        bool usedTranslationMerge = false;
        uint64_t recordedDrawCount = 0u;
        uint64_t recordedPassCount = 0u;
        uint64_t commandRecordingFailureCount = 0u;
        uint32_t parallelRecordingWorkerCount = 0u;
        std::string lastCommandRecordingFailure;
        std::string parallelFallbackReason;

        [[nodiscard]] std::string Describe() const
        {
            return " frameId=" + std::to_string(frameId) +
                " submitted=" + std::to_string(submittedSuccessfully) +
                " dropped=" + std::to_string(droppedForSwapchainRecreation) +
                " draws=" + std::to_string(recordedDrawCount) +
                " passes=" + std::to_string(recordedPassCount) +
                " recordingFailures=" + std::to_string(commandRecordingFailureCount) +
                " reason=" + lastCommandRecordingFailure +
                " fallback=" + parallelFallbackReason;
        }
    };

    class RhiFrameObservations final
    {
    public:
        explicit RhiFrameObservations(NLS::Render::Context::Driver& driver, const bool collectCompleted = false)
            : m_state(std::make_shared<State>())
        {
            NLS::Render::Context::DriverTestAccess::SetRhiFrameObserver(driver,
                [state = m_state, collectCompleted](const NLS::Render::Context::RhiSubmissionFrame& frame)
                {
                    // Keep metadata only: observation must not extend a GPU resource lease.
                    RhiFrameObservation observation {
                        frame.frameId, frame.offscreenOnly, frame.usedExternalOutputBridge,
                        frame.submittedSuccessfully, frame.droppedForSwapchainRecreation,
                        frame.usedTranslationMerge, frame.recordedDrawCount, frame.recordedPassCount,
                        frame.commandRecordingFailureCount, frame.parallelRecordingWorkerCount,
                        frame.lastCommandRecordingFailure, frame.parallelFallbackReason
                    };
                    std::lock_guard lock(state->mutex);
                    ++state->count;
                    state->latest = observation.Describe();
                    if (state->firstFailure.empty() &&
                        ((!observation.submittedSuccessfully && !observation.droppedForSwapchainRecreation) ||
                         observation.commandRecordingFailureCount != 0u))
                    {
                        state->firstFailure = state->latest;
                    }
                    if (collectCompleted)
                        state->completed.push_back(std::move(observation));
                });
        }

        [[nodiscard]] std::string DescribeFailures() const
        {
            std::lock_guard lock(m_state->mutex);
            return m_state->firstFailure;
        }

        [[nodiscard]] std::string DescribeLatest() const
        {
            std::lock_guard lock(m_state->mutex);
            return m_state->latest;
        }

        [[nodiscard]] size_t GetObservedCount() const
        {
            std::lock_guard lock(m_state->mutex);
            return m_state->count;
        }

        std::vector<RhiFrameObservation> TakeCompleted()
        {
            std::lock_guard lock(m_state->mutex);
            return std::exchange(m_state->completed, {});
        }

    private:
        struct State
        {
            std::mutex mutex;
            size_t count = 0u;
            std::string firstFailure;
            std::string latest;
            std::vector<RhiFrameObservation> completed;
        };
        std::shared_ptr<State> m_state;
    };

    /**
     * Forward renderer that exposes the recorded-draw static-base cache size.
     *
     * The cache is keyed partly by mesh identity, so proxy churn that creates new
     * meshes inserts new entries every frame. The engine caps the cache, and that
     * cap is the property under test: an unbounded cache turns each later lookup
     * into a longer scan and the frame rate degrades without ever failing.
     */
    class ProbeForwardSceneRenderer final
        : public NLS::Engine::Rendering::ForwardSceneRenderer
    {
    public:
        explicit ProbeForwardSceneRenderer(NLS::Render::Context::Driver& driver)
            : ForwardSceneRenderer(driver)
        {
        }

        [[nodiscard]] size_t StaticBaseCacheSize() const
        {
#if defined(NLS_ENABLE_TEST_HOOKS)
            return NLS::Render::Core::ABaseRenderer::GetPreparedRecordedDrawStaticBaseCacheSizeForTesting();
#else
            return 0u;
#endif
        }
    };

    /**
     * Number of frames the stress loop runs.
     *
     * The default keeps a routine `NullusPerformanceTests` invocation bounded; the
     * full acceptance run (10,000 frames) is requested explicitly:
     *
     *   $env:NLS_THREADED_STRESS_FRAMES = '10000'
     *   $env:NLS_THREADED_STRESS_TIMEOUT_SECONDS = '2400'
     */
    size_t ResolveStressFrameCount()
    {
        constexpr size_t kDefaultFrameCount = 240u;
        constexpr size_t kMaxFrameCount = 200000u;

        const char* const configured = std::getenv("NLS_THREADED_STRESS_FRAMES");
        if (configured == nullptr || *configured == '\0')
            return kDefaultFrameCount;

        const long long parsed = std::strtoll(configured, nullptr, 10);
        if (parsed <= 0)
            return kDefaultFrameCount;

        return (std::min)(static_cast<size_t>(parsed), kMaxFrameCount);
    }

    std::chrono::seconds ResolveStressTimeout()
    {
        constexpr long long kDefaultTimeoutSeconds = 900ll;

        const char* const configured = std::getenv("NLS_THREADED_STRESS_TIMEOUT_SECONDS");
        if (configured == nullptr || *configured == '\0')
            return std::chrono::seconds(kDefaultTimeoutSeconds);

        const long long parsed = std::strtoll(configured, nullptr, 10);
        if (parsed <= 0)
            return std::chrono::seconds(kDefaultTimeoutSeconds);

        return std::chrono::seconds(parsed);
    }

    std::chrono::seconds ResolveStressMinimumDuration()
    {
        const char* const configured = std::getenv("NLS_THREADED_STRESS_MIN_SECONDS");
        if (configured == nullptr || *configured == '\0')
            return std::chrono::seconds::zero();
        const long long parsed = std::strtoll(configured, nullptr, 10);
        return std::chrono::seconds((std::clamp)(parsed, 0ll, 86400ll));
    }

#if defined(_MSC_VER) && defined(_DEBUG)
    /**
     * Env-gated heap validation.
     *
     * The scene-mutation run can corrupt the debug heap over thousands of frames,
     * and the crash surface is far away from the write that caused it. Walking the
     * heap after each individual step localises the corruption to the operation
     * that produced it; the check is far too slow to leave on by default.
     *
     * Enabled with NLS_THREADED_STRESS_HEAP_CHECK=1.
     */
    bool HeapCheckEnabled()
    {
        static const bool enabled = []
        {
            const char* const configured = std::getenv("NLS_THREADED_STRESS_HEAP_CHECK");
            return configured != nullptr && *configured != '\0' && *configured != '0';
        }();
        return enabled;
    }
#endif

    /**
     * Bisect switch for the scene-mutation run.
     *
     * Each mutation is individually disableable so a failure can be attributed to
     * proxy churn, scene swapping or resize instead of to "the test in general".
     */
    bool MutationEnabled(const char* const variableName)
    {
        const char* const configured = std::getenv(variableName);
        return configured == nullptr || *configured == '\0' || *configured == '0';
    }

    /**
     * Selects the graphics backend for the scene-mutation run.
     *
     * NLS_THREADED_STRESS_BACKEND=none runs the identical churn against the no-device
     * path, which separates "the scene mutation is unsafe by itself" from "the scene
     * mutation races the threaded capture/render path".
     * NLS_THREADED_STRESS_BACKEND=vulkan runs the identical churn on the Vulkan
     * backend so the backend acceptance matrix is not limited to DX12.
     * NLS_THREADED_STRESS_BACKEND=metal runs the same matrix on the Apple Metal
     * backend, which is the only backend the macOS acceptance host can present on.
     */
    bool UseNullBackend()
    {
        const char* const configured = std::getenv("NLS_THREADED_STRESS_BACKEND");
        return configured != nullptr && std::string { configured } == "none";
    }

    /**
     * Resolves the requested stress backend.
     *
     * An unset value follows the platform default from DriverSettings, so the
     * Windows host keeps running DX12 while a macOS host runs Metal without an
     * extra export. Unknown values fall back to the same platform default.
     */
    NLS::Render::Settings::EGraphicsBackend ResolveStressBackend()
    {
        using NLS::Render::Settings::EGraphicsBackend;
        const char* const configured = std::getenv("NLS_THREADED_STRESS_BACKEND");
        if (configured == nullptr || *configured == '\0')
            return NLS::Render::Settings::DriverSettings {}.graphicsBackend;

        const std::string requested { configured };
        if (requested == "none")
            return EGraphicsBackend::NONE;
        if (requested == "vulkan")
            return EGraphicsBackend::VULKAN;
        if (requested == "metal")
            return EGraphicsBackend::METAL;
        if (requested == "dx12")
            return EGraphicsBackend::DX12;
        return NLS::Render::Settings::DriverSettings {}.graphicsBackend;
    }

    /**
     * Headless NoAPI surface for backends whose device creation needs a window.
     *
     * Vulkan refuses to create a device when no platform window is supplied, so
     * the runs that never present still create one. DX12 and the null backend
     * leave both pointers empty.
     */
    struct StressPlatformWindow
    {
        std::unique_ptr<NLS::Context::Device> device;
        std::unique_ptr<NLS::Windowing::Window> window;

        void Ensure(
            const NLS::Render::Settings::EGraphicsBackend backend,
            const char* const title)
        {
            if (backend != NLS::Render::Settings::EGraphicsBackend::VULKAN)
                return;

            device = std::make_unique<NLS::Context::Device>(
                NLS::Windowing::Settings::DeviceSettings {});
            NLS::Windowing::Settings::WindowSettings windowSettings;
            windowSettings.title = title;
            windowSettings.width = 320u;
            windowSettings.height = 180u;
            windowSettings.focused = false;
            windowSettings.clientAPI = NLS::Windowing::Settings::WindowClientAPI::NoAPI;
            window = std::make_unique<NLS::Windowing::Window>(*device, windowSettings);
        }

        [[nodiscard]] void* GetGlfwWindow() const
        {
            return window != nullptr ? window->GetGlfwWindow() : nullptr;
        }
    };

#if defined(_MSC_VER) && defined(_DEBUG)
    /** Routes CRT reports to stderr so a check fails without aborting the process. */
    void PrepareHeapCheckReporting()
    {
        if (!HeapCheckEnabled())
            return;
        (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    }

    bool HeapIsIntact()
    {
        return _CrtCheckMemory() != 0;
    }
#endif
}

/**
 * Sustained-frame acceptance run for the threaded rendering path.
 *
 * What this covers that the unit suites cannot:
 *  - Frame-slot resources are recycled across a long run, so a leak in the
 *    retirement hand-off shows up as growth instead of passing a short test.
 *  - The render thread, the RHI lane and the game thread overlap for thousands
 *    of frames, which is the only way the capture/resolve claim lifetime and the
 *    un-run-builder teardown race become observable.
 *  - Normal frame admission waits for capacity before capture. Every iteration
 *    must publish a frame and no failed retirement may be hidden by later success.
 *
 * The test fails fast on device loss or a queue failure so a device error is
 * reported with the frame index that produced it instead of surfacing later as a
 * mysterious retirement-count assertion.
 */
TEST(ThreadedRenderingStressPerformanceTests, SustainedForwardFramesRetireWithoutResourceGrowth)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    const ScopedVulkanValidationCheck validationCheck;
#endif
    NLS::Render::Settings::DriverSettings settings;
    const char* const configuredRhiMode = std::getenv("NLS_THREADED_STRESS_RHI_MODE");
    const auto requestedRhiMode = NLS::Render::Settings::ParseRhiThreadModePreference(
        configuredRhiMode != nullptr ? configuredRhiMode : "backend_default");
    ASSERT_TRUE(requestedRhiMode.has_value()) << "Invalid NLS_THREADED_STRESS_RHI_MODE";
    settings.rhiThreadMode = *requestedRhiMode;
    settings.graphicsBackend = ResolveStressBackend();
    settings.enableExplicitRHI = true;
    settings.framesInFlight = 2u;

    StressPlatformWindow platformWindow;
    platformWindow.Ensure(settings.graphicsBackend, "Nullus Threading Stress");
    settings.platformWindow = platformWindow.GetGlfwWindow();

    // Declared before the renderer so the renderer is destroyed first and the
    // driver can still drain the frames the renderer published.
    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    if (driver->GetActiveGraphicsBackend() != settings.graphicsBackend)
        GTEST_SKIP() << "The requested backend is unavailable on this host.";

    const ScopedDriverService driverService(*driver);
    const RhiFrameObservations observations(*driver);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::HasExplicitRHI(*driver));
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver))
        << "The threaded path is what this stress run measures.";

    const size_t frameCount = ResolveStressFrameCount();
    const std::chrono::seconds timeout = ResolveStressTimeout();

    std::unique_ptr<ProbeForwardSceneRenderer> renderer;
    ASSERT_NO_THROW(renderer = std::make_unique<ProbeForwardSceneRenderer>(*driver));
    ASSERT_NE(renderer, nullptr);

    NLS::Render::Resources::Shader* shader =
        NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
            "App/Assets/Engine/Shaders/Standard.hlsl");
    if (shader == nullptr)
        GTEST_SKIP() << "The built-in Standard shader is unavailable on this host.";

    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    // Real geometry: an offscreen frame that records no passes is intentionally
    // not submitted, so a draw-less scene would never reach the GPU queue and
    // would make the retention checks vacuous.
    std::unique_ptr<NLS::Render::Resources::Mesh> mesh(
        new NLS::Render::Resources::Mesh(
            std::vector<NLS::Render::Geometry::Vertex>{
                [] { NLS::Render::Geometry::Vertex v{}; v.position[0] = -0.5f; v.normals[2] = 1.0f; return v; }(),
                [] { NLS::Render::Geometry::Vertex v{}; v.position[0] = 0.5f; v.normals[2] = 1.0f; return v; }(),
                [] { NLS::Render::Geometry::Vertex v{}; v.position[1] = 0.5f; v.normals[2] = 1.0f; return v; }()
            },
            std::vector<uint32_t>{0u, 1u, 2u},
            0u,
            NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
            {{0.0f, 0.0f, 0.0f}, 1.0f}));

    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("LightActor").AddComponent<NLS::Engine::Components::LightComponent>();
    {
        auto& object = scene.CreateGameObject("MeshActor");
        auto* meshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
        meshFilter->SetMesh(mesh.get());
        meshRenderer->SetFrustumBehaviour(
            NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
        meshRenderer->FillWithMaterial(material);
        object.GetTransform()->SetWorldPosition({0.0f, 0.0f, -5.0f});
    }

    // A real offscreen target. Without one the frame targets the swapchain, and a
    // driver without a swapchain records nothing and fails every frame; the stress
    // harness would then measure the setup rather than the pipeline.
    NLS::Render::Buffers::Framebuffer outputBuffer(256u, 144u);
    ASSERT_NE(outputBuffer.GetExplicitTextureHandle(), nullptr)
        << "The offscreen target must be a real device resource for this run to mean anything.";

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    const ScopedFrameDrain frameDrain(*driver);
    const auto runStart = std::chrono::steady_clock::now();
    size_t maxRetainedResources = 0u;
    size_t peakInFlightDepth = 0u;
    uint64_t publishedFrames = 0u;
    size_t framesCompleted = 0u;

    for (size_t frameIndex = 0u; frameIndex < frameCount; ++frameIndex)
    {
        // Re-attached every frame: EndFrame clears the descriptor set, which is how
        // the renderer is driven in production too.
        renderer->AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
            scene,
            std::nullopt,
            nullptr
        });
        renderer->BeginFrame(frameDescriptor);
        renderer->DrawFrame();
        renderer->EndFrame();
        ASSERT_TRUE(renderer->WasLastThreadedFramePublished()) << "Publication failed at frame " << frameIndex;
        ++framesCompleted;

        // Production retires frames from its present/UI synchronisation points.
        // Draining here is the stress harness's stand-in for those, and it is what
        // lets the frame slots be reused instead of the run stalling on slots that
        // are legitimately still busy.
        if ((frameIndex % 8u) == 7u)
        {
            NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);
        }

        if ((frameIndex % 32u) == 31u)
        {
            // Progress marker: when a structured exception kills the run, the last
            // marker printed names the frame it died on.
            GTEST_LOG_(INFO) << "[ThreadedRenderingSceneStress] frame=" << frameIndex
                             << " staticBaseCache=" << renderer->StaticBaseCacheSize();
            const auto telemetry =
                NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);

            // Fail fast, attributed to the frame that produced the failure.
            ASSERT_FALSE(telemetry.deviceLostDetected)
                << "Device lost at frame " << frameIndex << ": " << telemetry.deviceLostReason;
            ASSERT_EQ(telemetry.queueOperationFailureCount, 0u)
                << "Queue failure at frame " << frameIndex << ": "
                << telemetry.lastQueueOperationFailure;
            ASSERT_FALSE(telemetry.unsafeGpuWorkQuarantined)
                << "GPU work quarantined at frame " << frameIndex << ": "
                << telemetry.unsafeGpuWorkQuarantineReason;
            ASSERT_EQ(telemetry.latestFailedRetiredFrameId, 0u)
                << "Failed retirement at frame " << frameIndex << observations.DescribeFailures();

            maxRetainedResources = (std::max)(
                maxRetainedResources,
                NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(*driver));
            NLS::Render::Context::FlushRenderingCommands(
                NLS::Render::Context::RhiFlushFlags::Gpu |
                NLS::Render::Context::RhiFlushFlags::ReleaseResources);
            ASSERT_EQ(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(*driver), 0u)
                << "Completed GPU work still owns frame resources at frame " << frameIndex;
            ASSERT_EQ(NLS::Render::Context::RhiCommandListExecutor::Get().GetPendingResourceReleaseCount(), 0u);
            ASSERT_EQ(NLS::Render::Context::RhiCommandListExecutor::Get().GetQuarantinedResourceReleaseCount(), 0u);
            peakInFlightDepth = (std::max)(peakInFlightDepth, telemetry.inFlightFrameCount);
            publishedFrames = telemetry.publishedFrameCount;
        }

        if ((frameIndex % 64u) == 63u)
        {
            const auto elapsed = std::chrono::steady_clock::now() - runStart;
            ASSERT_LT(elapsed, timeout)
                << "Stress run exceeded its " << timeout.count()
                << "s budget after " << frameIndex + 1u << " frames; the rendering "
                << "pipeline is not making progress.";
        }
    }

    // Retire everything before reading telemetry so the counters describe frames
    // that completed rather than frames still in flight.
    NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);
    NLS::Render::Context::FlushRenderingCommands(
        NLS::Render::Context::RhiFlushFlags::Gpu |
        NLS::Render::Context::RhiFlushFlags::ReleaseResources);

    const auto telemetry =
        NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
    const size_t retainedAfterDrain =
        NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(*driver);
    EXPECT_EQ(NLS::Render::Context::RhiCommandListExecutor::Get().GetPendingResourceReleaseCount(), 0u);
    EXPECT_EQ(NLS::Render::Context::RhiCommandListExecutor::Get().GetQuarantinedResourceReleaseCount(), 0u);
    const auto elapsed = std::chrono::steady_clock::now() - runStart;
    const double elapsedSeconds =
        std::chrono::duration<double>(elapsed).count();

    std::string summary =
        "frames=" + std::to_string(framesCompleted) +
        " published=" + std::to_string(telemetry.publishedFrameCount) +
        " successfullyRetired=" + std::to_string(telemetry.successfullyRetiredFrameCount) +
        " latestRetiredFrameId=" + std::to_string(telemetry.latestRetiredFrameId) +
        " latestFailedRetiredFrameId=" + std::to_string(telemetry.latestFailedRetiredFrameId) +
        " peakInFlight=" + std::to_string(peakInFlightDepth) +
        " maxRetainedResources=" + std::to_string(maxRetainedResources) +
        " retainedAfterDrain=" + std::to_string(retainedAfterDrain) +
        " elapsedSeconds=" + std::to_string(elapsedSeconds) +
        observations.DescribeLatest();

    EXPECT_FALSE(telemetry.deviceLostDetected) << summary << " " << telemetry.deviceLostReason;
    EXPECT_EQ(telemetry.queueOperationFailureCount, 0u) << summary;
    EXPECT_FALSE(telemetry.unsafeGpuWorkQuarantined) << summary;
    EXPECT_EQ(telemetry.publishedFrameCount, frameCount) << summary;
    EXPECT_EQ(observations.GetObservedCount(), frameCount) << summary;
    EXPECT_EQ(telemetry.successfullyRetiredFrameCount, frameCount) << summary;
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, 0u) << summary;
    EXPECT_GT(telemetry.latestRetiredFrameId, 0u)
        << "A sustained run must actually retire frames on the real device. " << summary;

    // Each slot owns several resource references; CPU retirement can precede the
    // GPU fence. Every periodic GPU flush above must release all of those owners.
    EXPECT_EQ(telemetry.inFlightFrameCount, 0u) << summary;
    EXPECT_EQ(retainedAfterDrain, 0u)
        << "Completed GPU work still owns frame resources after the final drain. " << summary;

    EXPECT_GT(publishedFrames, 0u) << summary;

    // Reported for the acceptance record rather than asserted: the frame budget is
    // exercised by wall-clock runs, not by a Debug-build unit threshold.
    GTEST_LOG_(INFO) << "[ThreadedRenderingStress] " << summary;

    mesh.reset();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

namespace
{
    /**
     * Builds a scene with a fixed number of visible primitives.
     *
     * Each actor owns its own mesh so adding and removing actors exercises the
     * proxy create/destroy path rather than only moving existing proxies.
     */
    struct StressSceneFixture
    {
        // Destruction order is the reverse of declaration, and it matters: the
        // scene must be torn down before the meshes its renderers point at, while
        // the retained meshes must outlive the scene that still references them.
        // Declaring the meshes first makes them the last to be released.
        std::vector<std::unique_ptr<NLS::Render::Resources::Mesh>> meshes;
        /**
         * Meshes whose actor is already destroyed but whose memory must survive.
         *
         * A frame the render thread is still resolving holds raw Mesh pointers from
         * the drawables it captured. Freeing a mesh as soon as its actor dies
         * therefore races that frame: the pointer in the captured drawable is read
         * after the allocation is gone. The engine has no GPU-fence-deferred release
         * for meshes yet, so the owner has to keep them alive until the frames that
         * referenced them have retired - which is what this list does.
         */
        std::vector<std::unique_ptr<NLS::Render::Resources::Mesh>> pendingRelease;
        std::vector<NLS::Engine::GameObject*> actors;
        NLS::Render::Resources::Material* material = nullptr;
        NLS::Engine::SceneSystem::Scene scene;

        void AddActors(const size_t count, const size_t firstIndex)
        {
            for (size_t offset = 0u; offset < count; ++offset)
            {
                const size_t index = firstIndex + offset;
                // Constructed through a raw pointer: the vertex/index arguments are
                // braced lists, which make_unique's forwarding cannot deduce.
                std::unique_ptr<NLS::Render::Resources::Mesh> mesh(
                    new NLS::Render::Resources::Mesh(
                    std::vector<NLS::Render::Geometry::Vertex>{
                        [] { NLS::Render::Geometry::Vertex v{}; v.position[0] = -0.5f; v.normals[2] = 1.0f; return v; }(),
                        [] { NLS::Render::Geometry::Vertex v{}; v.position[0] = 0.5f; v.normals[2] = 1.0f; return v; }(),
                        [] { NLS::Render::Geometry::Vertex v{}; v.position[1] = 0.5f; v.normals[2] = 1.0f; return v; }()
                    },
                    std::vector<uint32_t>{0u, 1u, 2u},
                    0u,
                    NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
                    {{0.0f, 0.0f, 0.0f}, 1.0f}));

                auto& object = scene.CreateGameObject("StressActor" + std::to_string(index));
                auto* meshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
                auto* meshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
                meshFilter->SetMesh(mesh.get());
                meshRenderer->SetFrustumBehaviour(
                    NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
                meshRenderer->FillWithMaterial(*material);
                object.GetTransform()->SetWorldPosition({
                    static_cast<float>(index % 5u) * 0.6f - 1.2f,
                    static_cast<float>(index % 3u) * 0.5f - 0.5f,
                    -5.0f - static_cast<float>(index % 4u) * 0.4f
                });

                actors.push_back(&object);
                meshes.push_back(std::move(mesh));
            }
        }

        void RemoveOldest(const size_t count)
        {
            for (size_t offset = 0u; offset < count && !actors.empty(); ++offset)
            {
                (void)scene.DestroyGameObject(*actors.front());
                actors.erase(actors.begin());
                pendingRelease.push_back(std::move(meshes.front()));
                meshes.erase(meshes.begin());
            }
        }

        /** Frees the retained meshes. Only safe once the frames using them retired. */
        void ReleasePendingMeshes() { pendingRelease.clear(); }
    };
}

/**
 * Acceptance run for the scene-mutation and resize paths under threading.
 *
 * The sustained-frame run above holds the scene still, so it never touches the
 * paths that hand game-thread state to the render thread. This run mutates those
 * while frames are in flight:
 *
 *  - Proxies are created and destroyed continuously while admission keeps the
 *    capture/claim bookkeeping within its depth limit.
 *  - The active scene is swapped mid-run, which invalidates every proxy identity
 *    the render scene had cached.
 *  - Resizes are requested while frames are in flight, which is the path that has
 *    to drain GPU fences before the frame contexts may be reset.
 *
 * Every iteration must publish successfully without failed retirements, and
 * every mutation must leave resources and outstanding captures bounded.
 *
 * REGRESSION HISTORY
 * ------------------
 * This run used to die intermittently with an access violation (0xC0000005) or
 * debug-heap corruption once the frame count reached the thousands. Bisecting with
 * the NLS_THREADED_STRESS_DISABLE_* switches below showed that proxy churn was the
 * necessary ingredient - swap and resize alone did not reproduce it - and the
 * captured fault was in SceneStreamingResidency::Commit, reached from
 * ResolveSceneParse.
 *
 * The cause was that the resolved-builder invocation was not serialized: the render
 * thread's tick and the game thread's synchronous drain both resolved frames, and
 * with two slots in scene-build state both could resolve the same renderer at once,
 * corrupting the unguarded renderer state. That mutex has since been deleted: scene
 * builds now run only on the render thread through the main command pipe. There
 * are no slot claim/resolving flags or game-thread scene-build entry points.
 *
 * Keep the switches: they are what made the bisect possible, and they localise any
 * future failure to a single mutation.
 */
TEST(ThreadedRenderingStressPerformanceTests, SceneMutationAndResizeKeepFrameResourcesBounded)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    const ScopedVulkanValidationCheck validationCheck;
#endif
    NLS::Render::Settings::DriverSettings settings;
    const char* const configuredRhiMode = std::getenv("NLS_THREADED_STRESS_RHI_MODE");
    const auto requestedRhiMode = NLS::Render::Settings::ParseRhiThreadModePreference(
        configuredRhiMode != nullptr ? configuredRhiMode : "backend_default");
    ASSERT_TRUE(requestedRhiMode.has_value()) << "Invalid NLS_THREADED_STRESS_RHI_MODE";
    settings.rhiThreadMode = *requestedRhiMode;
    const bool useNullBackend = UseNullBackend();
    settings.graphicsBackend = ResolveStressBackend();
    settings.enableExplicitRHI = !useNullBackend;
    settings.framesInFlight = 2u;

    StressPlatformWindow platformWindow;
    platformWindow.Ensure(settings.graphicsBackend, "Nullus Scene Mutation Stress");
    settings.platformWindow = platformWindow.GetGlfwWindow();

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    if (driver->GetActiveGraphicsBackend() != settings.graphicsBackend)
        GTEST_SKIP() << "The requested backend is unavailable on this host.";

    const ScopedDriverService driverService(*driver);
    const RhiFrameObservations observations(*driver);
    if (!useNullBackend)
    {
        ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::HasExplicitRHI(*driver));
        ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver));
    }

    const size_t frameCount = ResolveStressFrameCount();
    const std::chrono::seconds timeout = ResolveStressTimeout();
    const auto minimumDuration = ResolveStressMinimumDuration();
    ASSERT_LT(minimumDuration, timeout);

    std::unique_ptr<ProbeForwardSceneRenderer> renderer;
    ASSERT_NO_THROW(renderer = std::make_unique<ProbeForwardSceneRenderer>(*driver));
    ASSERT_NE(renderer, nullptr);

    NLS::Render::Resources::Shader* shader =
        NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
            "App/Assets/Engine/Shaders/Standard.hlsl");
    if (shader == nullptr)
        GTEST_SKIP() << "The built-in Standard shader is unavailable on this host.";

    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    // Two scenes so the active identity can be swapped, plus a light in each so
    // the lighting descriptor is refreshed by the swap as well.
    StressSceneFixture firstScene;
    StressSceneFixture secondScene;
    firstScene.material = &material;
    secondScene.material = &material;
    firstScene.scene.CreateGameObject("LightActor").AddComponent<NLS::Engine::Components::LightComponent>();
    secondScene.scene.CreateGameObject("LightActor").AddComponent<NLS::Engine::Components::LightComponent>();
    firstScene.AddActors(4u, 0u);
    secondScene.AddActors(4u, 0u);

    NLS::Render::Buffers::Framebuffer outputBuffer(256u, 144u);
    if (!useNullBackend)
    {
        ASSERT_NE(outputBuffer.GetExplicitTextureHandle(), nullptr)
            << "The offscreen target must be a real device resource for this run to mean anything.";
    }

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &outputBuffer);

    const ScopedFrameDrain frameDrain(*driver);
    const auto runStart = std::chrono::steady_clock::now();
    size_t maxRetainedResources = 0u;
    size_t maxInFlightDepth = 0u;
    size_t framesCompleted = 0u;
    size_t sceneSwaps = 0u;
    size_t maxStaticBaseCacheSize = 0u;

    bool useSecondScene = false;
    StressSceneFixture* activeScene = &firstScene;
    uint32_t nextResizeWidth = 256u;

#if defined(_MSC_VER) && defined(_DEBUG)
    PrepareHeapCheckReporting();
#endif

    for (size_t frameIndex = 0u;
         frameIndex < frameCount || std::chrono::steady_clock::now() - runStart < minimumDuration;
         ++frameIndex)
    {
#if defined(_MSC_VER) && defined(_DEBUG)
        const auto checkHeap = [frameIndex](const char* const step)
        {
            if (!HeapCheckEnabled())
                return;
            ASSERT_TRUE(HeapIsIntact())
                << "Debug heap is already corrupt after frame " << frameIndex
                << " step '" << step << "'.";
        };

        // Flushed step markers: an access violation kills the process without
        // draining the buffered gtest log, so the marker has to reach the file
        // before the step runs for the last one to be readable.
        const auto markStep = [frameIndex](const char* const step)
        {
            static const bool enabled = []
            {
                const char* const configured = std::getenv("NLS_THREADED_STRESS_STEP_MARKERS");
                return configured != nullptr && *configured != '\0' && *configured != '0';
            }();
            if (!enabled)
                return;
            std::fprintf(stderr, "[step] frame=%zu %s\n", frameIndex, step);
            std::fflush(stderr);
        };
#else
        const auto checkHeap = [](const char*) {};
        const auto markStep = [](const char*) {};
#endif

        markStep("add-descriptor");
        renderer->AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
            activeScene->scene,
            std::nullopt,
            nullptr
        });
        markStep("begin");
        renderer->BeginFrame(frameDescriptor);
        markStep("draw");
        renderer->DrawFrame();
        markStep("end");
        renderer->EndFrame();
        ASSERT_TRUE(renderer->WasLastThreadedFramePublished()) << "Publication failed at frame " << frameIndex;
        ++framesCompleted;
        markStep("post-end");
        checkHeap("begin-draw-end");

        // Churn the proxy set: destroy two, then create two so the visible count
        // stays roughly constant while identities keep changing.
        if ((frameIndex % 5u) == 4u && MutationEnabled("NLS_THREADED_STRESS_DISABLE_CHURN"))
        {
            markStep("churn");
            activeScene->RemoveOldest(2u);
            activeScene->AddActors(2u, frameIndex);
            markStep("post-churn");
            checkHeap("proxy-churn");
        }

        // Swap the active scene, which invalidates every cached proxy identity.
        if ((frameIndex % 37u) == 36u && MutationEnabled("NLS_THREADED_STRESS_DISABLE_SWAP"))
        {
            markStep("swap");
            useSecondScene = !useSecondScene;
            activeScene = useSecondScene ? &secondScene : &firstScene;
            ++sceneSwaps;
            markStep("post-swap");
            checkHeap("scene-swap");
        }

        // Request a resize while frames may still be in flight. The resize path
        // must drain the GPU fences before the frame contexts are reset, and it is
        // allowed to defer when it cannot get the submission lane - so the check is
        // that it eventually applies, not that it applies immediately.
        if ((frameIndex % 53u) == 52u && MutationEnabled("NLS_THREADED_STRESS_DISABLE_RESIZE"))
        {
            markStep("resize");
            nextResizeWidth = nextResizeWidth == 256u ? 320u : 256u;
            driver->ResizePlatformSwapchain(nextResizeWidth, 144u);
            markStep("post-resize");
            checkHeap("resize-request");
        }

        if ((frameIndex % 8u) == 7u)
        {
            markStep("drain");
            NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);
            markStep("post-drain");
            checkHeap("drain");
        }

        if ((frameIndex % 32u) == 31u)
        {
            const auto telemetry =
                NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
            ASSERT_FALSE(telemetry.deviceLostDetected)
                << "Device lost at frame " << frameIndex << ": " << telemetry.deviceLostReason;
            ASSERT_EQ(telemetry.queueOperationFailureCount, 0u)
                << "Queue failure at frame " << frameIndex << ": "
                << telemetry.lastQueueOperationFailure;
            ASSERT_FALSE(telemetry.unsafeGpuWorkQuarantined)
                << "GPU work quarantined at frame " << frameIndex << ": "
                << telemetry.unsafeGpuWorkQuarantineReason;
            ASSERT_EQ(telemetry.latestFailedRetiredFrameId, 0u)
                << "Failed retirement at frame " << frameIndex << observations.DescribeFailures();

            maxRetainedResources = (std::max)(
                maxRetainedResources,
                NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(*driver));
            NLS::Render::Context::FlushRenderingCommands(
                NLS::Render::Context::RhiFlushFlags::Gpu |
                NLS::Render::Context::RhiFlushFlags::ReleaseResources);
            ASSERT_EQ(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(*driver), 0u)
                << "Completed GPU work still owns frame resources at frame " << frameIndex;
            ASSERT_EQ(NLS::Render::Context::RhiCommandListExecutor::Get().GetPendingResourceReleaseCount(), 0u);
            ASSERT_EQ(NLS::Render::Context::RhiCommandListExecutor::Get().GetQuarantinedResourceReleaseCount(), 0u);
            firstScene.ReleasePendingMeshes();
            secondScene.ReleasePendingMeshes();
            maxInFlightDepth = (std::max)(maxInFlightDepth, telemetry.inFlightFrameCount);
            maxStaticBaseCacheSize =
                (std::max)(maxStaticBaseCacheSize, renderer->StaticBaseCacheSize());
        }

        if ((frameIndex % 64u) == 63u)
        {
            const auto elapsed = std::chrono::steady_clock::now() - runStart;
            ASSERT_LT(elapsed, timeout)
                << "Scene-mutation stress exceeded its " << timeout.count()
                << "s budget after " << frameIndex + 1u << " frames.";
        }
    }

    // Let a deferred resize land before measuring: it is only allowed to apply when
    // the frame ring is empty, which is exactly the state a final drain produces.
    NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);
    driver->ResizePlatformSwapchain(256u, 144u);
    NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);
    NLS::Render::Context::FlushRenderingCommands(
        NLS::Render::Context::RhiFlushFlags::Gpu |
        NLS::Render::Context::RhiFlushFlags::ReleaseResources);

    // Only now is it safe to free the meshes whose actors were destroyed during the
    // run: every frame that could still hold a pointer to them has retired.
    EXPECT_EQ(NLS::Render::Context::RhiCommandListExecutor::Get().GetPendingResourceReleaseCount(), 0u);
    EXPECT_EQ(NLS::Render::Context::RhiCommandListExecutor::Get().GetQuarantinedResourceReleaseCount(), 0u);
    firstScene.ReleasePendingMeshes();
    secondScene.ReleasePendingMeshes();

    const auto telemetry =
        NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
    const size_t retainedAfterDrain =
        NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(*driver);
    const auto elapsed = std::chrono::steady_clock::now() - runStart;

    std::string summary =
        "frames=" + std::to_string(framesCompleted) +
        " sceneSwaps=" + std::to_string(sceneSwaps) +
        " published=" + std::to_string(telemetry.publishedFrameCount) +
        " successfullyRetired=" + std::to_string(telemetry.successfullyRetiredFrameCount) +
        " latestRetiredFrameId=" + std::to_string(telemetry.latestRetiredFrameId) +
        " latestFailedRetiredFrameId=" + std::to_string(telemetry.latestFailedRetiredFrameId) +
        " maxInFlight=" + std::to_string(maxInFlightDepth) +
        " maxStaticBaseCache=" + std::to_string(maxStaticBaseCacheSize) +
        " maxRetainedResources=" + std::to_string(maxRetainedResources) +
        " retainedAfterDrain=" + std::to_string(retainedAfterDrain) +
        " minimumDurationSeconds=" + std::to_string(minimumDuration.count()) +
        " elapsedSeconds=" + std::to_string(std::chrono::duration<double>(elapsed).count());

    EXPECT_FALSE(telemetry.deviceLostDetected) << summary;
    EXPECT_EQ(telemetry.queueOperationFailureCount, 0u) << summary;
    EXPECT_FALSE(telemetry.unsafeGpuWorkQuarantined) << summary;
    EXPECT_GE(framesCompleted, frameCount) << summary;
    EXPECT_GE(elapsed, minimumDuration) << summary;
    EXPECT_EQ(telemetry.publishedFrameCount, framesCompleted) << summary;
    EXPECT_EQ(observations.GetObservedCount(), framesCompleted) << summary;
    EXPECT_EQ(telemetry.successfullyRetiredFrameCount, framesCompleted) << summary;
    EXPECT_EQ(telemetry.latestFailedRetiredFrameId, 0u) << summary << observations.DescribeFailures();
    EXPECT_GT(telemetry.latestRetiredFrameId, 0u) << summary;
    EXPECT_EQ(telemetry.inFlightFrameCount, 0u) << summary;

    EXPECT_EQ(retainedAfterDrain, 0u)
        << "Completed GPU work still owns frame resources after scene mutation and resize. " << summary;

    // The static-base cache is keyed partly by mesh identity, and this run creates
    // a new mesh every few frames, so the cache is under continuous pressure. It
    // must stay capped: growth here is what turns later lookups into long scans and
    // makes the frame rate decay over a long session.
    EXPECT_LE(maxStaticBaseCacheSize, 1024u)
        << "The recorded-draw static-base cache grew past its cap. " << summary;

    GTEST_LOG_(INFO) << "[ThreadedRenderingSceneStress] " << summary;

    // Leave the renderer alive until the end: it owns the frame-object binding
    // provider the in-flight builders point at.
    renderer.reset();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(ThreadedRenderingSwapchainPerformanceTests, ForwardPresentAndResizeRetireOnOrderedLane)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    const ScopedVulkanValidationCheck validationCheck;
#endif
#if (defined(_WIN32) || defined(__APPLE__)) && defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Context::Device windowDevice(NLS::Windowing::Settings::DeviceSettings {});
    NLS::Windowing::Settings::WindowSettings windowSettings;
    windowSettings.title = "Nullus Forward Swapchain Validation";
    windowSettings.width = 320u;
    windowSettings.height = 180u;
    windowSettings.focused = false;
    windowSettings.clientAPI = NLS::Windowing::Settings::WindowClientAPI::NoAPI;
    NLS::Windowing::Window window(windowDevice, windowSettings);

    NLS::Render::Settings::DriverSettings settings;
    const char* const configuredRhiMode = std::getenv("NLS_THREADED_STRESS_RHI_MODE");
    const auto requestedRhiMode = NLS::Render::Settings::ParseRhiThreadModePreference(
        configuredRhiMode != nullptr ? configuredRhiMode : "backend_default");
    ASSERT_TRUE(requestedRhiMode.has_value()) << "Invalid NLS_THREADED_STRESS_RHI_MODE";
    settings.rhiThreadMode = *requestedRhiMode;
    settings.graphicsBackend = ResolveStressBackend();
    settings.enableExplicitRHI = true;
    settings.framesInFlight = 2u;
    settings.platformWindow = window.GetGlfwWindow();

    size_t resizeCount = 0u;
    NLS::Render::Context::Driver driver(settings);
    if (driver.GetActiveGraphicsBackend() != settings.graphicsBackend)
        GTEST_SKIP() << "The requested backend is unavailable on this host.";
    const ScopedDriverService driverService(driver);
    RhiFrameObservations observations(driver, true);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(driver));
    ASSERT_TRUE(driver.CreatePlatformSwapchain(
        window.GetGlfwWindow(), window.GetNativeWindowHandle(), 320u, 180u, false, 2u));
    driver.SetSwapchainWillResizeCallback([&resizeCount] { ++resizeCount; });
    auto* const impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);
    ASSERT_NE(impl->explicitSwapchain, nullptr);

    const auto destroyShader = [](NLS::Render::Resources::Shader* shader)
    {
        if (shader != nullptr)
            (void)NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader);
    };
    const std::unique_ptr<NLS::Render::Resources::Shader, decltype(destroyShader)> shader(
        NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
            "App/Assets/Engine/Shaders/Standard.hlsl"), destroyShader);
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader.get());
    NLS::Render::Resources::Material transparentMaterial(shader.get());
    transparentMaterial.SetBlendable(true);
    StressSceneFixture scene;
    scene.material = &material;
    scene.scene.CreateGameObject("LightActor").AddComponent<NLS::Engine::Components::LightComponent>();
    scene.AddActors(4u, 0u);
    scene.material = &transparentMaterial;
    scene.AddActors(4u, 4u);
    ProbeForwardSceneRenderer renderer(driver);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;
    // No external framebuffer: this must acquire and present a native backbuffer.
    const ScopedFrameDrain frameDrain(driver);
    const size_t frameCount = ResolveStressFrameCount();
    const auto timeout = ResolveStressTimeout();
    const auto runStart = std::chrono::steady_clock::now();
    auto& executor = NLS::Render::Context::RhiCommandListExecutor::Get();
    uint64_t lastPresentSequence = executor.GetPresentSequence();
    uint32_t maxParallelWorkers = 0u;
    std::string parallelFallback;
    size_t observedSwapchainSubmissions = 0u;
    size_t observedTranslationMerges = 0u;
    // The scene/presented-output comparison runs entirely inside one frame.
    // Vulkan gives the swapchain image to the presentation engine at Present, so a
    // post-Present readback of that image is not a synchronizable operation: it may
    // only be read while the frame still owns it. Both the scene color target and
    // the swapchain image are therefore read back through the frame's own
    // post-submit, pre-Present readback requests.
    std::shared_ptr<NLS::Render::Context::PostSubmitTextureReadbackState> sceneReadbackState;
    std::shared_ptr<NLS::Render::Context::PostSubmitTextureReadbackState> swapchainReadbackState;
    std::vector<uint8_t> scenePixels;
    std::vector<uint8_t> outputPixels;
    uint32_t readbackWidth = 0u;
    uint32_t readbackHeight = 0u;

#if defined(_MSC_VER) && defined(_DEBUG)
    PrepareHeapCheckReporting();
#endif
    for (size_t frameIndex = 0u; frameIndex < frameCount; ++frameIndex)
    {
        SCOPED_TRACE(frameIndex);
        windowDevice.PollEvents();
        renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
            scene.scene, std::nullopt, nullptr });
        renderer.BeginFrame(frameDescriptor);
        renderer.DrawFrame();
        renderer.EndFrame();
        ASSERT_TRUE(renderer.WasLastThreadedFramePublished()) << observations.DescribeFailures();

        const bool resizeFrame = (frameIndex % 32u) == 31u;
        if (resizeFrame)
        {
            frameDescriptor.renderWidth = frameDescriptor.renderWidth == 320u ? 384u : 320u;
            frameDescriptor.renderHeight = frameDescriptor.renderWidth == 320u ? 180u : 216u;
            window.SetSize(static_cast<uint16_t>(frameDescriptor.renderWidth),
                static_cast<uint16_t>(frameDescriptor.renderHeight));
            driver.ResizePlatformSwapchain(frameDescriptor.renderWidth, frameDescriptor.renderHeight);
        }
        if ((frameIndex % 8u) == 7u || frameIndex + 1u == frameCount)
        {
            ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver));
            NLS::Render::Context::RenderCommandFence presentFence;
            presentFence.BeginFence(NLS::Render::Context::RenderCommandSyncDepth::Swapchain);
            ASSERT_TRUE(presentFence.WaitFor(std::chrono::seconds(30)));
            ASSERT_GT(presentFence.GetTargetPresentSequence(), lastPresentSequence);
            ASSERT_GE(executor.GetPresentSequence(), presentFence.GetTargetPresentSequence());
            lastPresentSequence = executor.GetPresentSequence();
            ASSERT_FALSE(executor.HasFailed()) << executor.GetFailureDiagnostic();

            // A swapchain image belongs to the presentation engine between
            // Present and the next acquire, so Vulkan cannot read it back after
            // Present without tripping WRITE_AFTER_PRESENT; it has to use the
            // frame's own post-submit, pre-Present readback. DX12 keeps reading
            // the resolved backbuffer directly, which its Present model allows.
            const bool useInFrameSwapchainReadback =
                driver.GetActiveGraphicsBackend() == NLS::Render::Settings::EGraphicsBackend::VULKAN;
            if (useInFrameSwapchainReadback && frameIndex == 7u)
            {
                // Arm the paired readback for the next frame, then compare the two
                // destinations after that frame is drained below.
                readbackWidth = frameDescriptor.renderWidth;
                readbackHeight = frameDescriptor.renderHeight;
                scenePixels.assign(static_cast<size_t>(readbackWidth) * readbackHeight * 4u, 0u);
                outputPixels.assign(scenePixels.size(), 0u);
                sceneReadbackState = std::make_shared<NLS::Render::Context::PostSubmitTextureReadbackState>();
                swapchainReadbackState = std::make_shared<NLS::Render::Context::PostSubmitTextureReadbackState>();

                NLS::Render::Context::PostSubmitTextureReadbackRequest sceneRequest;
                sceneRequest.source = NLS::Render::Context::TextureReadbackSource::FrameSceneColor;
                sceneRequest.width = readbackWidth;
                sceneRequest.height = readbackHeight;
                sceneRequest.destination = scenePixels.data();
                sceneRequest.state = sceneReadbackState;
                renderer.SetNextFramePostSubmitTextureReadback(std::move(sceneRequest));

                NLS::Render::Context::PostSubmitTextureReadbackRequest swapchainRequest;
                swapchainRequest.source = NLS::Render::Context::TextureReadbackSource::FrameSwapchain;
                swapchainRequest.width = readbackWidth;
                swapchainRequest.height = readbackHeight;
                swapchainRequest.destination = outputPixels.data();
                swapchainRequest.state = swapchainReadbackState;
                renderer.SetNextFramePostSubmitTextureReadback(std::move(swapchainRequest));
            }
            else if (useInFrameSwapchainReadback && frameIndex == 15u)
            {
                ASSERT_NE(sceneReadbackState, nullptr);
                ASSERT_NE(swapchainReadbackState, nullptr);
                const auto waitForReadback = [](const auto& state, const char* label)
                {
                    std::shared_ptr<NLS::Render::RHI::RHICompletionToken> completion;
                    {
                        std::lock_guard lock(state->mutex);
                        EXPECT_TRUE(state->beginAttempted) << label << ": readback was never dispatched";
                        EXPECT_TRUE(state->beginSucceeded) << label << ": " << state->resultMessage;
                        EXPECT_EQ(state->resultCode, NLS::Render::RHI::RHIReadbackStatusCode::Success)
                            << label << ": " << state->resultMessage;
                        completion = state->completion;
                    }
                    // A backend whose readback completes inline (the default
                    // BeginReadPixels implementation) publishes the destination
                    // bytes before reporting success and has no token to wait on.
                    if (completion != nullptr)
                        EXPECT_TRUE(completion->Wait().Succeeded()) << label << ": readback did not complete";
                };
                waitForReadback(sceneReadbackState, "scene color");
                waitForReadback(swapchainReadbackState, "swapchain");

                const auto difference = std::mismatch(scenePixels.begin(), scenePixels.end(), outputPixels.begin());
                EXPECT_EQ(difference.first, scenePixels.end())
                    << "Swapchain output differs from the scene at byte " << std::distance(scenePixels.begin(), difference.first);
                size_t nonBackgroundPixels = 0u;
                for (size_t pixel = 4u; pixel < scenePixels.size(); pixel += 4u)
                    if (!std::equal(scenePixels.begin(), scenePixels.begin() + 4u, scenePixels.begin() + pixel))
                        ++nonBackgroundPixels;
                EXPECT_GT(nonBackgroundPixels, 0u) << "Scene/output equality must include actual rendered geometry.";
            }
            else if (!useInFrameSwapchainReadback && frameIndex == 7u)
            {
                const NLS::Render::RHI::RHIFrameContext* latest = nullptr;
                for (size_t slot = 0u; slot < impl->GetFrameContextCount(); ++slot)
                {
                    const auto& frame = impl->GetFrameContext(slot);
                    if (frame.sceneColorTexture != nullptr && (latest == nullptr || frame.frameIndex > latest->frameIndex))
                        latest = &frame;
                }
                ASSERT_NE(latest, nullptr);
                ASSERT_NE(latest->swapchainBackbufferView, nullptr);
                const auto backbuffer = NLS::Render::Context::DriverRendererAccess::ResolveReadbackTexture(driver);
                ASSERT_EQ(backbuffer, latest->swapchainBackbufferView->GetTexture());
                const auto width = frameDescriptor.renderWidth;
                const auto height = frameDescriptor.renderHeight;
                std::vector<uint8_t> sceneColorPixels(static_cast<size_t>(width) * height * 4u);
                std::vector<uint8_t> presentedPixels(sceneColorPixels.size());
                for (const auto& source : { std::make_pair(latest->sceneColorTexture, sceneColorPixels.data()),
                    std::make_pair(backbuffer, presentedPixels.data()) })
                {
                    const auto readback = NLS::Render::Context::DriverRendererAccess::ReadPixelsChecked(
                        driver, source.first, 0u, 0u, width, height,
                        NLS::Render::Settings::EPixelDataFormat::RGBA,
                        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE, source.second);
                    ASSERT_TRUE(readback.Succeeded()) << readback.message;
                }
                const auto difference = std::mismatch(
                    sceneColorPixels.begin(), sceneColorPixels.end(), presentedPixels.begin());
                EXPECT_EQ(difference.first, sceneColorPixels.end())
                    << "Swapchain output differs from the scene at byte "
                    << std::distance(sceneColorPixels.begin(), difference.first);
                size_t nonBackgroundPixels = 0u;
                for (size_t pixel = 4u; pixel < sceneColorPixels.size(); pixel += 4u)
                    if (!std::equal(
                            sceneColorPixels.begin(), sceneColorPixels.begin() + 4u,
                            sceneColorPixels.begin() + pixel))
                        ++nonBackgroundPixels;
                EXPECT_GT(nonBackgroundPixels, 0u) << "Scene/output equality must include actual rendered geometry.";
            }

            const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
            ASSERT_FALSE(telemetry.deviceLostDetected) << telemetry.deviceLostReason;
            ASSERT_EQ(telemetry.queueOperationFailureCount, 0u) << telemetry.lastQueueOperationFailure;
            ASSERT_FALSE(telemetry.unsafeGpuWorkQuarantined) << telemetry.unsafeGpuWorkQuarantineReason;
            ASSERT_EQ(telemetry.latestFailedRetiredFrameId, 0u) << observations.DescribeFailures();
            // This is the only scenario in the suite that resizes a real platform
            // swapchain while frames are in flight, so a frame published against a
            // swapchain the surface just replaced is dropped instead of rendered. That
            // drop is expected output of a resize, not a failure: it is counted
            // separately, and no queue operation reports a failure for it.
            ASSERT_EQ(
                telemetry.successfullyRetiredFrameCount +
                    telemetry.droppedForSwapchainRecreationFrameCount,
                frameIndex + 1u)
                << observations.DescribeFailures();
            if (telemetry.droppedForSwapchainRecreationFrameCount != 0u)
            {
                // A drop is only legitimate when it is the resize that moved the
                // swapchain, so it can never appear before the first resize frame.
                ASSERT_GE(frameIndex, 31u) << observations.DescribeFailures();
            }
            const auto completedFrames = observations.TakeCompleted();
            ASSERT_FALSE(completedFrames.empty());
            ASSERT_EQ(observations.GetObservedCount(), frameIndex + 1u);
            for (const auto& submission : completedFrames)
            {
                SCOPED_TRACE(submission.frameId);
                if (submission.droppedForSwapchainRecreation)
                {
                    // A frame dropped because the resize moved the swapchain under it
                    // recorded and submitted nothing, so the recording-bearing
                    // assertions below do not apply to it. Everything it must not do -
                    // report a failure, leak a slot, submit unordered - is asserted
                    // elsewhere in this loop and in the retirement telemetry.
                    continue;
                }
                ASSERT_FALSE(submission.offscreenOnly);
                ASSERT_FALSE(submission.usedExternalOutputBridge);
                ASSERT_TRUE(submission.submittedSuccessfully);
                ASSERT_GT(submission.recordedDrawCount, 0u);
                ASSERT_GT(submission.recordedPassCount, 0u);
                ASSERT_EQ(submission.commandRecordingFailureCount, 0u) << submission.lastCommandRecordingFailure;
                maxParallelWorkers = (std::max)(maxParallelWorkers, submission.parallelRecordingWorkerCount);
                parallelFallback = submission.parallelFallbackReason;
                observedTranslationMerges += submission.usedTranslationMerge ? 1u : 0u;
                ++observedSwapchainSubmissions;
            }
            if (resizeFrame)
            {
                ASSERT_EQ(resizeCount, (frameIndex + 1u) / 32u);
                const auto& desc = impl->explicitSwapchain->GetDesc();
                ASSERT_EQ(desc.width, frameDescriptor.renderWidth);
                ASSERT_EQ(desc.height, frameDescriptor.renderHeight);
                const auto backbuffer = impl->explicitSwapchain->GetBackbufferView(0u);
                ASSERT_NE(backbuffer, nullptr);
                ASSERT_NE(backbuffer->GetTexture(), nullptr);
                ASSERT_EQ(backbuffer->GetTexture()->GetDesc().extent.width, desc.width);
                ASSERT_EQ(backbuffer->GetTexture()->GetDesc().extent.height, desc.height);
            }
#if defined(_MSC_VER) && defined(_DEBUG)
            if (HeapCheckEnabled())
                ASSERT_TRUE(HeapIsIntact());
#endif
            ASSERT_LT(std::chrono::steady_clock::now() - runStart, timeout);
        }
    }

    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryWaitForSubmittedGpuWork(driver));
    window.SetSize(416u, 234u);
    windowDevice.PollEvents();
    driver.ResizePlatformSwapchain(416u, 234u);
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver));
    ASSERT_EQ(impl->explicitSwapchain->GetDesc().width, 416u);
    ASSERT_EQ(impl->explicitSwapchain->GetDesc().height, 234u);
    ASSERT_EQ(resizeCount, frameCount / 32u + 1u);
    EXPECT_GT(observedSwapchainSubmissions, 0u);
    EXPECT_GT(maxParallelWorkers, 1u) << "The real native command path must execute on workers.";
    EXPECT_GT(observedTranslationMerges, 0u);
    EXPECT_EQ(NLS::Render::Context::DriverTestAccess::GetRetainedThreadedSubmitResourceCount(driver), 0u);
    NLS::Render::Context::FlushRenderingCommands(
        NLS::Render::Context::RhiFlushFlags::Gpu | NLS::Render::Context::RhiFlushFlags::ReleaseResources);
    EXPECT_EQ(NLS::Render::Context::RhiCommandListExecutor::Get().GetPendingResourceReleaseCount(), 0u);
    EXPECT_EQ(NLS::Render::Context::RhiCommandListExecutor::Get().GetQuarantinedResourceReleaseCount(), 0u);
    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.publishedFrameCount, frameCount);
    // Every published frame reaches a terminal state. Frames the resize dropped are
    // counted separately from the ones that rendered and submitted, and the total has
    // to account for all of them.
    EXPECT_EQ(
        telemetry.successfullyRetiredFrameCount +
            telemetry.droppedForSwapchainRecreationFrameCount,
        frameCount);
    EXPECT_LE(telemetry.droppedForSwapchainRecreationFrameCount, resizeCount)
        << "a resize may drop at most the frame that was in flight while it was applied";
    EXPECT_EQ(telemetry.inFlightFrameCount, 0u);
    GTEST_LOG_(INFO) << "[ThreadedRenderingSwapchainStress] frames=" << frameCount
        << " resized=" << resizeCount << " presentSequence=" << lastPresentSequence
        << " maxParallelWorkers=" << maxParallelWorkers << " fallback=" << parallelFallback
        << " translationMergeSamples=" << observedTranslationMerges
        << " elapsedSeconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();
#else
    GTEST_SKIP() << "Requires the Windows DX12 or Apple Metal backend with NLS_ENABLE_TEST_HOOKS.";
#endif
}
