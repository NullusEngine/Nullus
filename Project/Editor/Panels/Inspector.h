#pragma once

#include <UI/Panels/PanelWindow.h>

#include <string>

namespace NLS::Editor::Inspector
{
class InspectorView;
}

namespace NLS::Editor::Panels
{
class ComponentSearchPanel;

class Inspector : public NLS::UI::PanelWindow
{
public:
    /**
     * Constructor
     * @param p_title
     * @param p_opened
     * @param p_windowSettings
     */
    Inspector(
        const std::string& p_title,
        bool p_opened,
        const NLS::UI::PanelWindowSettings& p_windowSettings);

    /**
     * Destructor
     */
    ~Inspector();

    /**
     * Refresh the inspector
     */
    void Refresh();

private:
    NLS::Editor::Inspector::InspectorView* m_view = nullptr;
};
} // namespace NLS::Editor::Panels
