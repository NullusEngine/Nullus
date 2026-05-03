#include <Time/Clock.h>

#include "Core/Application.h"
#include "Core/ResizeRefreshPolicy.h"
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
    const Render::Settings::RenderDocSettings& p_renderDocSettings,
    bool p_enableThreadedRendering,
    bool p_enableRhiDebugValidation,
    const Render::Settings::EngineDiagnosticsSettings& p_diagnosticsSettings)
    : m_context(p_projectPath, p_projectName, p_backendOverride, p_renderDocSettings, p_enableThreadedRendering, p_enableRhiDebugValidation, p_diagnosticsSettings), m_editor(m_context)
{
    const auto tickWhileResizing = [this]()
    {
        if (!IsRunning())
            return;

        SyncPlatformSwapchainToFramebufferSize();

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

        clock.Update();
    }
}

void Editor::Core::Application::TickFrame(float p_deltaTime, bool p_pollEvents)
{
    if (m_isTicking)
        return;

    m_isTicking = true;

    if (p_pollEvents)
    {
        m_isPollingEvents = true;
        m_editor.PreUpdate();
        m_isPollingEvents = false;
    }

    RunEditorFrame(p_deltaTime);

    const bool resizeCursorActive =
        m_context.uiManager != nullptr && IsResizeCursor(m_context.uiManager->GetMouseCursor());
    const bool primaryMouseDown =
        m_context.inputManager != nullptr &&
        m_context.inputManager->GetMouseButtonState(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT) ==
            Windowing::Inputs::EMouseButtonState::MOUSE_DOWN;

    if (ShouldRunResizeFollowUpFrame(false, resizeCursorActive, primaryMouseDown))
    {
        m_isResizeTicking = true;
        RunEditorFrame(0.0f);
        m_isResizeTicking = false;
    }

    m_isTicking = false;
}

void Editor::Core::Application::TickResizeFrame()
{
    if (m_isResizeTicking)
        return;

    m_isResizeTicking = true;
    SyncPlatformSwapchainToFramebufferSize();
    RunEditorFrame(0.0f);
    if (ShouldRunResizeFollowUpFrame(true, false, false))
    {
        // The first resize frame updates ImGui's layout state.
        // The follow-up frame re-renders views against the new panel sizes.
        RunEditorFrame(0.0f);
    }
    m_isResizeTicking = false;
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

bool Editor::Core::Application::IsRunning() const
{
    return !m_context.window->ShouldClose();
}

void Editor::Core::Application::RunEditorFrame(const float deltaTime)
{
    m_editor.Update(deltaTime);
    m_editor.PostUpdate();
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
