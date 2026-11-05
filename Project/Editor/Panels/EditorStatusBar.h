#pragma once

#include <UI/Panels/PanelViewportBar.h>
#include <functional>
#include <string>

namespace NLS::Editor::Core { class EditorTaskProgressService; }

namespace NLS::Editor::Panels
{
    class Console;
    class EditorStatusBar : public UI::PanelViewportBar
    {
    public:
        EditorStatusBar();
        void SetSources(Console& console, Core::EditorTaskProgressService& tasks,
            std::function<void()> openConsole, std::function<void()> openTasks,
            std::function<float()> frameRate);
        static std::string SingleLine(const std::string& text);
        static std::string Elide(const std::string& text, float width);

    protected:
        void DrawBarContent() override;

    private:
        Console* m_console = nullptr;
        Core::EditorTaskProgressService* m_tasks = nullptr;
        std::function<void()> m_openConsole;
        std::function<void()> m_openTasks;
        std::function<float()> m_frameRate;
    };
}
