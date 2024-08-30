#include "UI/Widgets/Texts/TextClickable.h"

namespace NLS::UI::Widgets
{
TextClickable::TextClickable(const std::string& p_content)
    : Text(p_content)
{
}

void TextClickable::_Draw_Impl()
{
    bool useless = false;

    if (ImGui::Selectable((content + m_widgetID).c_str(), &useless, ImGuiSelectableFlags_AllowDoubleClick))
    {
        if (ImGui::IsMouseDoubleClicked(0))
        {
            DoubleClickedEvent.Invoke();
        }
        else
        {
            ClickedEvent.Invoke();
        }
    }
}
} // namespace NLS::UI::Widgets
