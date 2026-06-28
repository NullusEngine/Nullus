#include <Time/Clock.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include <Profiling/Profiler.h>

#include "Core/ApplicationIdleFramePolicy.h"
#include "Core/Application.h"
#include "Core/AssetFileWatcher.h"
#include "Core/ResizeRefreshPolicy.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/EditorStartupAssetPreimport.h"
#include "Rendering/BaseSceneRenderer.h"
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
        m_context.RefreshRuntimeAssetDatabaseFromArtifactDB();
        NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(m_context.shaderManager);
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

    m_editor = std::make_unique<Editor>(m_context);
    m_editor->AdoptStartupAssetWatchers(
        std::move(startupEngineAssetsWatcher),
        std::move(startupProjectAssetsWatcher));

    const auto tickWhileResizing = [this]()
    {
        if (!IsRunning())
            return;

        if (m_context.window != nullptr && m_context.window->IsNativeResizeInProgress())
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

        if (!ShouldTickResizeImmediately(m_isTicking, m_isPollingEvents, m_isResizeTicking))
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
        if (!imported)
        {
            throw std::runtime_error(
                "Startup watcher asset preimport failed; editor UI will not open until changed assets are imported.");
        }
    };

    runStartupWatcherPreimport("Importing startup asset changes", 0.94f);
    m_context.RefreshRuntimeAssetDatabaseFromArtifactDB();
    m_editor->RefreshProjectAssetBrowser();
    m_context.PresentStartupProgressFrame("Rendering first editor frame", 0.96f);
    RunEditorFrame(0.0f);
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
    if (!finalStartupWatcherImport)
    {
        throw std::runtime_error(
            "Final startup watcher asset preimport failed; editor UI will not open until changed assets are imported.");
    }
    m_context.RefreshRuntimeAssetDatabaseFromArtifactDB();
    m_context.PresentStartupProgressFrame("Opening editor", 1.0f);
    NLS_LOG_INFO("[Startup] CompleteStartupProgress begin");
    m_context.CompleteStartupProgress();
    NLS_LOG_INFO("[Startup] CompleteStartupProgress end");
}

Editor::Core::Application::~Application()
{
    m_context.ShutdownThreadedRendering();
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

    NLS_PROFILE_NAMED_SCOPE("Application::TickFrame");

    if (p_pollEvents)
    {
        const ScopedBoolFlag pollingScope(m_isPollingEvents);
        {
            NLS_PROFILE_NAMED_SCOPE("Application::EditorPreUpdate");
            m_editor->PreUpdate();
        }
    }
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

    {
        NLS_PROFILE_NAMED_SCOPE("Application::SyncPlatformSwapchainToFramebufferSize");
        SyncPlatformSwapchainToFramebufferSize();
    }
    {
        NLS_PROFILE_NAMED_SCOPE("Application::ResizeFrame");
        RunEditorFrame(0.0f);
    }
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

    {
        NLS_PROFILE_NAMED_SCOPE("Application::EditorUpdate");
        m_editor->Update(deltaTime);
    }
    {
        NLS_PROFILE_NAMED_SCOPE("Application::EditorPostUpdate");
        m_editor->PostUpdate();
    }
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
