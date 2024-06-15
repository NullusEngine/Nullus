#include "UI/Widgets/Buttons/ButtonArrow.h"

namespace NLS
{
UI::Widgets::Buttons::ButtonArrow::ButtonArrow(ImGuiDir p_direction)
    : direction(p_direction)
{
}

void UI::Widgets::Buttons::ButtonArrow::_Draw_Impl()
{
    if (ImGui::ArrowButton(m_widgetID.c_str(), direction))
        ClickedEvent.Invoke();
}
} // namespace NLS
