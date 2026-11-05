#pragma once

#include <UI/Panels/PanelWindow.h>
#include "Core/EditorTaskProgressService.h"

namespace NLS::Editor::Panels
{
class BackgroundTasks : public UI::PanelWindow
{
public:
    BackgroundTasks(const std::string& title, bool opened, const UI::PanelWindowSettings& settings,
        Core::EditorTaskProgressService& tasks);
protected:
    void OnBeforeDrawWidgets() override;
private:
    Core::EditorTaskProgressService& m_tasks;
};
}
