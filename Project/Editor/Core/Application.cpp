#include <Time/Clock.h>

#include "Core/Application.h"
#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"
namespace NLS
{
Editor::Core::Application::Application(const std::string& p_projectPath, const std::string& p_projectName)
    : m_context(p_projectPath, p_projectName), m_editor(m_context)
{
    const bool allowImmediateResizeTick =
        m_context.windowSettings.clientAPI == Windowing::Settings::WindowClientAPI::OpenGL;

    const auto tickWhileResizing = [this]()
    {
        if (!IsRunning())
            return;

        if (m_context.windowSettings.clientAPI == Windowing::Settings::WindowClientAPI::OpenGL)
            m_context.window->MakeCurrentContext();

        if (m_isTicking || m_isResizeTicking)
        {
            QueueResizeTick();
            return;
        }

        TickResizeFrame();
    };

    const auto handleResizeEvent = [this, allowImmediateResizeTick, tickWhileResizing]()
    {
        if (allowImmediateResizeTick)
        {
            tickWhileResizing();
            return;
        }

        QueueResizeTick();
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

    m_editor.Update(p_deltaTime);
    m_editor.PostUpdate();

    m_isTicking = false;
}

void Editor::Core::Application::TickResizeFrame()
{
    if (m_isResizeTicking)
        return;

    m_isResizeTicking = true;
    m_editor.Update(0.0f);
    m_editor.PostUpdate();
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
} // namespace NLS
