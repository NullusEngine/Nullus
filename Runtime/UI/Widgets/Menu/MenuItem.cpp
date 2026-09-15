#include "UI/Widgets/Menu/MenuItem.h"
#include "ImGui/imgui.h"

namespace NLS::UI::Widgets
{
MenuItem::MenuItem(const std::string& p_name, const std::string& p_shortcut, bool p_checkable, bool p_checked)
    : DataWidget(m_selected), name(p_name), shortcut(p_shortcut), checkable(p_checkable), checked(p_checked)
{
}

void MenuItem::_Draw_Impl()
{
    bool previousValue = checked;

    if (ImGui::MenuItem((name + m_widgetID).c_str(), shortcut.c_str(), checkable ? &checked : nullptr, enabled))
        ClickedEvent.Invoke();

    if (!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip.c_str());

    if (checked != previousValue)
    {
        ValueChangedEvent.Invoke(checked);
        this->NotifyChange();
    }
}

} // namespace NLS
