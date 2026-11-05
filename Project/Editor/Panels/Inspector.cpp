#include "Panels/Inspector.h"

#include "Inspector/InspectorView.h"

namespace NLS::Editor::Panels
{
Inspector::Inspector(
    const std::string& title,
    const bool opened,
    const NLS::UI::PanelWindowSettings& windowSettings)
    : NLS::UI::PanelWindow(title, opened, windowSettings)
{
    m_view = &CreateWidget<NLS::Editor::Inspector::InspectorView>();
}

Inspector::~Inspector() = default;
bool Inspector::CanExecuteComponentCommand(const std::string& id, int32_t componentId) const
{ return m_view && m_view->CanExecuteComponentCommand(id, componentId); }
bool Inspector::ExecuteComponentCommand(const std::string& id, int32_t componentId)
{ return m_view && m_view->ExecuteComponentCommand(id, componentId); }

void Inspector::Refresh()
{
    if (m_view != nullptr)
        m_view->Refresh();
}
} // namespace NLS::Editor::Panels
