#include "UI/Internal/Converter.h"

#include "UI/Widgets/Buttons/ButtonSmall.h"

namespace NLS::UI::Widgets
{
ButtonSmall::ButtonSmall(const std::string& p_label)
    : label(p_label)
{
    auto& style = ImGui::GetStyle();

    idleBackgroundColor = Internal::Converter::ToColor(style.Colors[ImGuiCol_Button]);
    hoveredBackgroundColor = Internal::Converter::ToColor(style.Colors[ImGuiCol_ButtonHovered]);
    clickedBackgroundColor = Internal::Converter::ToColor(style.Colors[ImGuiCol_ButtonActive]);
    textColor = Internal::Converter::ToColor(style.Colors[ImGuiCol_Text]);
}

void ButtonSmall::_Draw_Impl()
{
    auto& style = ImGui::GetStyle();

    auto defaultIdleColor = style.Colors[ImGuiCol_Button];
    auto defaultHoveredColor = style.Colors[ImGuiCol_ButtonHovered];
    auto defaultClickedColor = style.Colors[ImGuiCol_ButtonActive];
    auto defaultTextColor = style.Colors[ImGuiCol_Text];

    style.Colors[ImGuiCol_Button] = UI::Internal::Converter::ToImVec4(idleBackgroundColor);
    style.Colors[ImGuiCol_ButtonHovered] = UI::Internal::Converter::ToImVec4(hoveredBackgroundColor);
    style.Colors[ImGuiCol_ButtonActive] = UI::Internal::Converter::ToImVec4(clickedBackgroundColor);
    style.Colors[ImGuiCol_Text] = UI::Internal::Converter::ToImVec4(textColor);

    if (ImGui::SmallButton((label + m_widgetID).c_str()))
        ClickedEvent.Invoke();

    style.Colors[ImGuiCol_Button] = defaultIdleColor;
    style.Colors[ImGuiCol_ButtonHovered] = defaultHoveredColor;
    style.Colors[ImGuiCol_ButtonActive] = defaultClickedColor;
    style.Colors[ImGuiCol_Text] = defaultTextColor;
}

} // namespace NLS
