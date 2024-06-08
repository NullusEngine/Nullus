#include <Time/Clock.h>

#include "Core/Application.h"

namespace NLS
{
Editor::Core::Application::Application(const std::string& p_projectPath, const std::string& p_projectName)
    : m_context(p_projectPath, p_projectName), m_editor(m_context)
{
}

Editor::Core::Application::~Application()
{
}

void Editor::Core::Application::Run()
{
    Time::Clock clock;

    while (IsRunning())
    {
        m_editor.PreUpdate();
        m_editor.Update(clock.GetDeltaTime());
        m_editor.PostUpdate();

        clock.Update();
    }
}

bool Editor::Core::Application::IsRunning() const
{
    return !m_context.window->ShouldClose();
}
} // namespace NLS
