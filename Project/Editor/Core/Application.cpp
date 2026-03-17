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
    m_context.window->RefreshEvent.AddListener([this]()
    {
        if (!IsRunning() || m_isTicking)
            return;

        TickFrame(0.0f, false);
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

        clock.Update();
    }
}

void Editor::Core::Application::TickFrame(float p_deltaTime, bool p_pollEvents)
{
    if (m_isTicking)
        return;

    m_isTicking = true;

    if (p_pollEvents)
        m_editor.PreUpdate();

    m_editor.Update(p_deltaTime);
    m_editor.PostUpdate();

    m_isTicking = false;
}

bool Editor::Core::Application::IsRunning() const
{
    return !m_context.window->ShouldClose();
}
} // namespace NLS
