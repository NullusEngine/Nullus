#include <Time/Clock.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include <Profiling/Profiler.h>
#include <Jobs/JobSystem.h>

#include "Core/ApplicationIdleFramePolicy.h"
#include "Core/Application.h"
#include "Core/AssetFileWatcher.h"
#include "Core/EditorActions.h"
#include "Core/ResizeRefreshPolicy.h"
#include "Core/StartupSceneReadyGate.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/EditorStartupAssetPreimport.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Settings/EditorSettings.h"
#include "Windowing/Inputs/EMouseButton.h"
#include "Windowing/Inputs/EMouseButtonState.h"
#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"
namespace NLS
{
namespace
{
constexpr auto kStartupSceneRendererResourceProgressLogInterval = std::chrono::seconds(1);
constexpr auto kStartupSceneRendererResourceProgressDialogInterval = std::chrono::milliseconds(250);

class ScopedBoolFlag final
{
public:
    explicit ScopedBoolFlag(bool& flag)
        : m_flag(flag)
    {
        m_flag = true;
    }

    ~ScopedBoolFlag()
    {
        m_flag = false;
    }

    ScopedBoolFlag(const ScopedBoolFlag&) = delete;
    ScopedBoolFlag& operator=(const ScopedBoolFlag&) = delete;

private:
    bool& m_flag;
};

bool IsResizeCursor(const ImGuiMouseCursor cursor)
{
    return cursor == ImGuiMouseCursor_ResizeEW ||
        cursor == ImGuiMouseCursor_ResizeNS ||
        cursor == ImGuiMouseCursor_ResizeNWSE ||
        cursor == ImGuiMouseCursor_ResizeNESW ||
        cursor == ImGuiMouseCursor_ResizeAll;
}

NLS::Editor::Core::StartupSceneRendererResourcePendingCounts GetStartupSceneRendererResourcePendingCounts()
{
    const auto snapshot = NLS::Editor::Core::GetSceneLoadRendererResourceReadinessSnapshot();
    NLS::Editor::Core::StartupSceneRendererResourcePendingCounts counts;
    counts.taskCount = snapshot.pendingTaskCount;
    counts.textureLoadCount = snapshot.pendingTextureLoadCount;
    counts.activeStateCount = snapshot.activeStateCount;
    return counts;
}
}

std::string Editor::Core::FormatStartupSceneRendererResourceDegradedOpenDiagnostic(
    const StartupSceneRendererResourceWaitResult& result)
{
    const char* reason = result.timeoutReason == StartupSceneRendererResourceTimeoutReason::Stalled
        ? "stalled"
        : "hard-limit";
    return
        "[Startup] WaitForStartupSceneRendererResources status=degraded-open failOpen=true "
        "reason=" + std::string(reason) + " elapsedMs=" + std::to_string(result.elapsed.count()) +
        " frames=" + std::to_string(result.frameCount) +
        " pendingTasks=" + std::to_string(result.pendingCounts.taskCount) +
        " pendingTextureLoads=" + std::to_string(result.pendingCounts.textureLoadCount) +
        " activeStates=" + std::to_string(result.pendingCounts.activeStateCount);
}

Editor::Core::Application::Application(const std::string& p_projectPath, const std::string& p_projectName,
    std::optional<Render::Settings::EGraphicsBackend> p_backendOverride,
    std::optional<Render::Settings::RenderDocSettings> p_renderDocOverride,
    std::optional<Render::Settings::EngineDiagnosticsSettings> p_diagnosticsOverride)
    : m_context(p_projectPath, p_projectName, p_backendOverride, p_renderDocOverride, p_diagnosticsOverride)
{
    m_context.PresentStartupProgressFrame("Watching project assets", 0.88f);
    std::error_code projectAssetFolderError;
    std::filesystem::create_directories(m_context.projectAssetsPath, projectAssetFolderError);
    if (projectAssetFolderError)
    {
        throw std::runtime_error(
            "Failed to create project Assets folder before startup asset watching: " +
            projectAssetFolderError.message());
    }

    NLS::Editor::Core::AssetFileWatcher startupEngineAssetsWatcher;
    NLS::Editor::Core::AssetFileWatcher startupProjectAssetsWatcher;
    const auto engineWatcherStarted = startupEngineAssetsWatcher.Start(m_context.engineAssetsPath);
    const auto projectWatcherStarted = startupProjectAssetsWatcher.Start(m_context.projectAssetsPath);
    const auto watcherStartupReport = NLS::Editor::Assets::BuildAssetWatcherStartupReport(
        m_context.engineAssetsPath,
        engineWatcherStarted,
        m_context.projectAssetsPath,
        projectWatcherStarted);
    for (const auto& diagnostic : watcherStartupReport.diagnostics)
        NLS_LOG_WARNING(diagnostic.message);
    if (!watcherStartupReport.succeeded)
    {
        throw std::runtime_error(
            "Startup asset watcher failed; editor UI will not open because post-open asset changes could be missed.");
    }

    m_context.PresentStartupProgressFrame("Importing project assets", 0.90f);
    const auto startupPreimport = NLS::Editor::Assets::RunBlockingStartupAssetPreimport(
        {std::filesystem::path(m_context.projectPath)},
        [this](const NLS::Editor::Assets::ImportProgressEvent& event)
        {
            const float progress =
                0.90f + static_cast<float>(event.normalizedProgress) * 0.05f;
            m_context.PresentStartupProgressFrame(
                NLS::Editor::Assets::FormatStartupAssetPreimportProgressLabel(event),
                progress);
        });
    if (startupPreimport.succeeded && !startupPreimport.hadRunningJobsAfterCompletion)
    {
        if (startupPreimport.importedAssetCount > 0u)
        {
            m_context.RefreshRuntimeAssetDatabaseFromArtifactDB();
            NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(m_context.shaderManager, false);
        }
        NLS_LOG_INFO(
            "Startup asset preimport completed: " +
            std::to_string(startupPreimport.importedAssetCount) +
            " imported / " +
            std::to_string(startupPreimport.plannedAssetCount) +
            " planned.");
    }
    else
    {
        NLS_LOG_ERROR(
            "Startup asset preimport failed: " +
            std::to_string(startupPreimport.importedAssetCount) +
            " imported / " +
            std::to_string(startupPreimport.plannedAssetCount) +
            " planned.");
        for (const auto& diagnostic : startupPreimport.diagnostics)
        {
            NLS_LOG_ERROR(
                "Startup asset preimport diagnostic code=" +
                diagnostic.code +
                " path=" +
                diagnostic.path.generic_string() +
                " message=" +
                diagnostic.message);
        }
        if (startupPreimport.hadRunningJobsAfterCompletion)
            NLS_LOG_ERROR("Startup asset preimport returned with running jobs still active.");
        throw std::runtime_error(
            "Startup asset preimport failed; editor UI will not open until project assets are imported.");
    }

    auto startupStepBegin = std::chrono::steady_clock::now();
    auto logStartupStep =
        [&startupStepBegin](const char* step)
        {
            const auto now = std::chrono::steady_clock::now();
            NLS_LOG_INFO(
                std::string("[Startup] Application step ") +
                step +
                " elapsedMs=" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - startupStepBegin).count()));
            startupStepBegin = now;
        };

    m_editor = std::make_unique<Editor>(m_context);
    logStartupStep("EditorConstruction");
    m_editor->AdoptStartupAssetWatchers(
        std::move(startupEngineAssetsWatcher),
        std::move(startupProjectAssetsWatcher));
    logStartupStep("AdoptStartupAssetWatchers");

    const auto tickWhileResizing = [this]()
    {
        if (!IsRunning())
            return;

        const bool nativeResizeInProgress =
            m_context.window != nullptr && m_context.window->IsNativeResizeInProgress();
        if (nativeResizeInProgress)
        {
            const auto framebufferSize = m_context.window->GetFramebufferSize();
            if (m_hasLastNativeResizeTickSize &&
                m_lastNativeResizeTickSize.x == framebufferSize.x &&
                m_lastNativeResizeTickSize.y == framebufferSize.y)
            {
                return;
            }

            m_lastNativeResizeTickSize = framebufferSize;
            m_hasLastNativeResizeTickSize = true;
        }
        else
        {
            m_hasLastNativeResizeTickSize = false;
        }

        if (!ShouldTickResizeImmediately(
            nativeResizeInProgress,
            m_isTicking,
            m_isPollingEvents,
            m_isResizeTicking))
        {
            QueueResizeTick();
            return;
        }

        TickResizeFrame();
    };

    const auto handleResizeEvent = [tickWhileResizing]()
    {
        tickWhileResizing();
    };

    m_context.window->RefreshEvent.AddListener([handleResizeEvent]()
    {
        handleResizeEvent();
    });
    m_context.window->ResizeEvent.AddListener([handleResizeEvent](uint16_t, uint16_t)
    {
        handleResizeEvent();
    });
    m_context.window->FramebufferResizeEvent.AddListener([handleResizeEvent](uint16_t, uint16_t)
    {
        handleResizeEvent();
    });

    const auto runStartupWatcherPreimport = [this](const std::string& label, const float baseProgress)
    {
        m_context.PresentStartupProgressFrame(label, baseProgress);
        const auto imported = m_editor->RunStartupWatcherPreimport(
            [this, baseProgress](const NLS::Editor::Assets::ImportProgressEvent& event)
            {
                const float progress =
                    baseProgress + static_cast<float>(event.normalizedProgress) * 0.01f;
                m_context.PresentStartupProgressFrame(
                    NLS::Editor::Assets::FormatStartupAssetPreimportProgressLabel(event),
                    progress);
            });
        if (!imported.succeeded)
        {
            throw std::runtime_error(
                "Startup watcher asset preimport failed; editor UI will not open until changed assets are imported.");
        }
        return imported;
    };

    const auto startupWatcherPreimport = runStartupWatcherPreimport("Importing startup asset changes", 0.94f);
    logStartupStep("RunStartupWatcherPreimport");
    if (startupWatcherPreimport.requiresRuntimeAssetRefresh)
    {
        m_context.RefreshRuntimeAssetDatabaseFromArtifactDB();
        NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(m_context.shaderManager, false);
        logStartupStep("RefreshRuntimeAssetsAfterWatcherPreimport");
    }
    m_editor->RefreshProjectAssetBrowser();
    logStartupStep("RefreshProjectAssetBrowser");
    NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(m_context.shaderManager, false);
    logStartupStep("PreloadSceneFallbackShaderBeforeFirstFrame");
    m_context.PresentStartupProgressFrame("Rendering first editor frame", 0.96f);
    m_editor->LogNextUpdateStages();
    m_logNextEditorFrameStages = true;
    RunEditorFrame(0.0f);
    logStartupStep("FirstEditorFrame");
    m_context.PresentStartupProgressFrame("Importing final startup asset changes", 0.97f);
    const auto finalStartupWatcherImport = m_editor->CompleteStartupWatcherPreimportGate(
        [this](const NLS::Editor::Assets::ImportProgressEvent& event)
        {
            const float progress =
                0.97f + static_cast<float>(event.normalizedProgress) * 0.01f;
            m_context.PresentStartupProgressFrame(
            NLS::Editor::Assets::FormatStartupAssetPreimportProgressLabel(event),
            progress);
        });
    logStartupStep("CompleteStartupWatcherPreimportGate");
    if (!finalStartupWatcherImport.succeeded)
    {
        throw std::runtime_error(
            "Final startup watcher asset preimport failed; editor UI will not open until changed assets are imported.");
    }
    if (finalStartupWatcherImport.requiresRuntimeAssetRefresh)
    {
        m_context.RefreshRuntimeAssetDatabaseFromArtifactDB();
        NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(m_context.shaderManager);
        logStartupStep("RefreshRuntimeAssetsAfterFinalWatcherImport");
    }
    const auto finalizationStatus = FinalizeStartupSceneBeforeWindow(
        [this, &logStartupStep]()
        {
            if (!WaitForStartupSceneRendererResources())
            {
                logStartupStep("WaitForStartupSceneRendererResourcesCancelled");
                return false;
            }
            logStartupStep("WaitForStartupSceneRendererResources");
            return true;
        },
        [this, &logStartupStep]()
        {
            m_context.PresentStartupProgressFrame("Finalizing startup scene", 0.995f);
            RunEditorFrame(0.0f);
            logStartupStep("FinalStartupSceneReadyFrame");
        },
        [this]()
        {
            const bool drained =
                m_context.driver == nullptr ||
                NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(*m_context.driver);
            if (drained)
                NLS_LOG_INFO("[Startup] FinalStartupSceneReadyFrame submission drain complete");
            return drained;
        },
        [this, &logStartupStep]()
        {
            if (!WaitForStartupSceneRendererResources())
            {
                logStartupStep("StabilizeFinalStartupSceneReadyFrameCancelled");
                return false;
            }
            logStartupStep("StabilizeFinalStartupSceneReadyFrame");
            return true;
        },
        [this, &logStartupStep]()
        {
            const bool completed =
                m_context.driver == nullptr ||
                NLS::Render::Context::DriverRendererAccess::TryWaitForSubmittedGpuWork(*m_context.driver);
            if (completed)
            {
                NLS_LOG_INFO("[Startup] FinalStartupSceneReadyFrame drain complete");
                logStartupStep("DrainFinalStartupSceneReadyFrame");
            }
            return completed;
        });
    if (finalizationStatus == StartupSceneFinalizationStatus::Cancelled)
        return;
    if (finalizationStatus == StartupSceneFinalizationStatus::SubmissionDrainFailed)
    {
        throw std::runtime_error(
            "[Startup] FinalStartupSceneReadyFrame drain failed during submission; editor window remains hidden");
    }
    if (finalizationStatus == StartupSceneFinalizationStatus::GpuWaitFailed)
    {
        throw std::runtime_error(
            "[Startup] FinalStartupSceneReadyFrame final drain failed; editor window remains hidden");
    }
    m_context.PresentStartupProgressFrame("Opening editor", 1.0f);
    NLS_LOG_INFO("[Startup] CompleteStartupProgress begin");
    m_context.CompleteStartupProgress();
    m_editorWindowShown = true;
    NLS_LOG_INFO("[Startup] CompleteStartupProgress end");
    logStartupStep("CompleteStartupProgress");
}

Editor::Core::Application::~Application()
{
    m_context.ShutdownThreadedRendering();
}

bool Editor::Core::Application::DidShowEditorWindow() const
{
    return m_editorWindowShown &&
        m_context.window != nullptr &&
        m_context.window->IsVisible();
}

bool Editor::Core::Application::WaitForStartupSceneRendererResources()
{
    const auto initialPendingCounts = GetStartupSceneRendererResourcePendingCounts();
    if (!HasPendingStartupSceneRendererResources(initialPendingCounts))
    {
        NLS_LOG_INFO(
            "[Startup] WaitForStartupSceneRendererResources skipped "
            "pendingTasks=0 pendingTextureLoads=0 activeStates=0");
        return true;
    }

    NLS_LOG_INFO(
        "[Startup] WaitForStartupSceneRendererResources begin pendingTasks=" +
        std::to_string(initialPendingCounts.taskCount) +
        " pendingTextureLoads=" +
        std::to_string(initialPendingCounts.textureLoadCount) +
        " activeStates=" +
        std::to_string(initialPendingCounts.activeStateCount));

    const auto result = WaitForStartupSceneRendererResourcesUntilReady(
        []() { return std::chrono::steady_clock::now(); },
        [this]() { return IsRunning(); },
        []() { return GetStartupSceneRendererResourcePendingCounts(); },
        [this]()
        {
            if (m_context.device != nullptr)
                m_context.device->PollEvents();
            if (!IsRunning())
                return;
            RunEditorFrame(0.0f);
            FlushDeferredResizeTick();
        },
        [this]()
        {
            m_context.PresentStartupProgressFrame("Preparing startup scene resources", 0.985f);
        },
        [](const std::chrono::milliseconds elapsed,
            const uint32_t frameCount,
            const NLS::Editor::Core::StartupSceneRendererResourcePendingCounts& pendingCounts)
        {
            NLS_LOG_INFO(
                "[Startup] WaitForStartupSceneRendererResources progress elapsedMs=" +
                std::to_string(elapsed.count()) +
                " frames=" +
                std::to_string(frameCount) +
                " pendingTasks=" +
                std::to_string(pendingCounts.taskCount) +
                " pendingTextureLoads=" +
                std::to_string(pendingCounts.textureLoadCount) +
                " activeStates=" +
                std::to_string(pendingCounts.activeStateCount));
        },
        []()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        },
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ResolveStartupSceneRendererResourceHardTimeout(
                NLS::Base::Jobs::GetBackgroundJobWorkerCount())),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            kStartupSceneRendererResourceStallTimeout),
        std::chrono::duration_cast<std::chrono::milliseconds>(kStartupSceneRendererResourceProgressLogInterval),
        kStartupSceneRendererResourceProgressDialogInterval);

    if (result.status == StartupSceneRendererResourceWaitStatus::Ready)
    {
        NLS_LOG_INFO(
            "[Startup] WaitForStartupSceneRendererResources end elapsedMs=" +
            std::to_string(result.elapsed.count()) +
            " frames=" +
            std::to_string(result.frameCount) +
            " pendingTasks=0 pendingTextureLoads=0 activeStates=0");
        return true;
    }

    if (result.status == StartupSceneRendererResourceWaitStatus::Timeout)
    {
        NLS_LOG_WARNING(FormatStartupSceneRendererResourceDegradedOpenDiagnostic(result));
        return true;
    }

    NLS_LOG_INFO(
        "[Startup] WaitForStartupSceneRendererResources cancelled because the editor window is closing frames=" +
        std::to_string(result.frameCount));
    return false;
}

void Editor::Core::Application::Run()
{
    Time::Clock clock;

    while (IsRunning())
    {
        TickFrame(clock.GetDeltaTime(), true);
        FlushDeferredResizeTick();
        PaceIdleFrameIfNeeded();

        {
            NLS_PROFILE_NAMED_SCOPE("Application::ClockUpdate");
            clock.Update();
        }
    }
}

void Editor::Core::Application::TickFrame(float p_deltaTime, bool p_pollEvents)
{
    if (m_isTicking)
        return;

    const ScopedBoolFlag tickingScope(m_isTicking);
    if (m_editor != nullptr)
        m_editor->BeginProfilerFrame();
    if (!IsRunning())
        return;

    NLS_PROFILE_NAMED_SCOPE("Application::TickFrame");

    if (p_pollEvents)
    {
        const ScopedBoolFlag pollingScope(m_isPollingEvents);
        {
            NLS_PROFILE_NAMED_SCOPE("Application::EditorPreUpdate");
            m_editor->PreUpdate();
        }
    }
    if (!IsRunning())
        return;

    m_lastFrameHadTransientInput = false;
    if (m_context.inputManager != nullptr)
    {
        const auto mousePosition = m_context.inputManager->GetMousePosition();
        const bool mouseMoved =
            m_hasLastIdlePacingMousePosition &&
            (mousePosition.x != m_lastIdlePacingMousePosition.x ||
                mousePosition.y != m_lastIdlePacingMousePosition.y);
        m_lastFrameHadTransientInput =
            m_context.inputManager->HasTransientInputEvents() ||
            mouseMoved;
        m_lastIdlePacingMousePosition = mousePosition;
        m_hasLastIdlePacingMousePosition = true;
    }

    {
        NLS_PROFILE_NAMED_SCOPE("Application::RunEditorFrame");
        RunEditorFrame(p_deltaTime);
    }
    if (!IsRunning())
        return;

    const bool resizeCursorActive =
        m_context.uiManager != nullptr && IsResizeCursor(m_context.uiManager->GetMouseCursor());
    const bool primaryMouseDown =
        m_context.inputManager != nullptr &&
        m_context.inputManager->GetMouseButtonState(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT) ==
            Windowing::Inputs::EMouseButtonState::MOUSE_DOWN;

    if (ShouldRunResizeFollowUpFrame(false, resizeCursorActive, primaryMouseDown))
    {
        const ScopedBoolFlag resizeTickingScope(m_isResizeTicking);
        {
            NLS_PROFILE_NAMED_SCOPE("Application::ResizeFollowUpFrame");
            RunEditorFrame(0.0f);
        }
    }
}

void Editor::Core::Application::TickResizeFrame()
{
    if (m_isResizeTicking)
        return;

    const ScopedBoolFlag resizeTickingScope(m_isResizeTicking);
    if (m_editor != nullptr)
        m_editor->BeginProfilerFrame();
    if (!IsRunning())
        return;

    {
        NLS_PROFILE_NAMED_SCOPE("Application::SyncPlatformSwapchainToFramebufferSize");
        SyncPlatformSwapchainToFramebufferSize();
    }
    {
        NLS_PROFILE_NAMED_SCOPE("Application::ResizeFrame");
        RunEditorFrame(0.0f);
    }
    if (!IsRunning())
        return;

    if (ShouldRunResizeFollowUpFrame(true, false, false))
    {
        // The first resize frame updates ImGui's layout state.
        // The follow-up frame re-renders views against the new panel sizes.
        {
            NLS_PROFILE_NAMED_SCOPE("Application::ResizeFollowUpFrame");
            RunEditorFrame(0.0f);
        }
    }
}

void Editor::Core::Application::QueueResizeTick()
{
    m_pendingResizeTick = true;
}

void Editor::Core::Application::FlushDeferredResizeTick()
{
    while (m_pendingResizeTick && IsRunning())
    {
        m_pendingResizeTick = false;
        TickResizeFrame();
    }
}

void Editor::Core::Application::PaceIdleFrameIfNeeded()
{
    if (m_context.inputManager == nullptr)
        return;

    const bool primaryMouseDown =
        m_context.inputManager->GetMouseButtonState(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT) ==
        Windowing::Inputs::EMouseButtonState::MOUSE_DOWN;
    const bool secondaryMouseDown =
        m_context.inputManager->GetMouseButtonState(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_RIGHT) ==
        Windowing::Inputs::EMouseButtonState::MOUSE_DOWN;
    const bool middleMouseDown =
        m_context.inputManager->GetMouseButtonState(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_MIDDLE) ==
        Windowing::Inputs::EMouseButtonState::MOUSE_DOWN;
    const bool hasMouseButtonDown = primaryMouseDown || secondaryMouseDown || middleMouseDown;
    const bool profilerRecording =
        NLS::Base::Profiling::Profiler::IsEnabled() ||
        (m_editor != nullptr && m_editor->IsProfilerRecordingEnabled());

    if (!ShouldPaceIdleEditorFrame(
        NLS::Editor::Settings::EditorSettings::GetRuntimeSettingsObject().enablePowerSavingIdlePacing,
        m_lastFrameHadTransientInput,
        hasMouseButtonDown,
        m_pendingResizeTick,
        m_isResizeTicking,
        profilerRecording))
    {
        return;
    }

    NLS_PROFILE_NAMED_SCOPE("Application::IdleFramePacing");
    std::this_thread::sleep_for(std::chrono::milliseconds(GetIdleEditorFramePacingMilliseconds()));
}

bool Editor::Core::Application::IsRunning() const
{
    return !m_context.window->ShouldClose();
}

void Editor::Core::Application::RunEditorFrame(const float deltaTime)
{
    if (m_editor == nullptr)
        return;

    const bool logFrameStages = m_logNextEditorFrameStages;
    m_logNextEditorFrameStages = false;
    auto frameStageBegin = std::chrono::steady_clock::now();
    auto logFrameStage =
        [&frameStageBegin, logFrameStages](const char* stage)
        {
            if (!logFrameStages)
                return;

            const auto now = std::chrono::steady_clock::now();
            NLS_LOG_INFO(
                std::string("[Startup] FirstEditorFrame stage ") +
                stage +
                " elapsedMs=" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - frameStageBegin).count()));
            frameStageBegin = now;
        };

    {
        NLS_PROFILE_NAMED_SCOPE("Application::EditorUpdate");
        m_editor->Update(deltaTime);
    }
    logFrameStage("EditorUpdate");
    {
        NLS_PROFILE_NAMED_SCOPE("Application::EditorPostUpdate");
        m_editor->PostUpdate();
    }
    logFrameStage("EditorPostUpdate");
}

void Editor::Core::Application::SyncPlatformSwapchainToFramebufferSize()
{
    if (m_context.window == nullptr || m_context.driver == nullptr)
        return;

    // Do not rely on resize listener invocation order. Sync the swapchain to the
    // latest framebuffer size before rendering any frame that depends on it.
    const auto framebufferSize = m_context.window->GetFramebufferSize();
    if (framebufferSize.x <= 0.0f || framebufferSize.y <= 0.0f)
        return;

    m_context.driver->ResizePlatformSwapchain(
        static_cast<uint32_t>(framebufferSize.x),
        static_cast<uint32_t>(framebufferSize.y));
}
} // namespace NLS
