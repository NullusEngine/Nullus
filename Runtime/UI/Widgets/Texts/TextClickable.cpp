#include "UI/Widgets/Texts/TextClickable.h"
#include <cfloat>

namespace NLS::UI::Widgets
{
TextClickable::TextClickable(const std::string& p_content)
    : Text(p_content)
{
}

void TextClickable::_Draw_Impl()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.29f, 0.40f, 0.56f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.24f, 0.36f, 0.52f, 0.78f));

    if (ImGui::Selectable((content + m_widgetID).c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, 0.0f)))
    {
        selected = true;
        if (ImGui::IsMouseDoubleClicked(0))
        {
            DoubleClickedEvent.Invoke();
        }
        else
        {
            ClickedEvent.Invoke();
        }
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}
} // namespace NLS::UI::Widgets
