#include "UI/Widgets/Texts/TextLabelled.h"

namespace NLS::UI::Widgets
{
TextLabelled::TextLabelled(const std::string& p_content, const std::string& p_label)
    : Text(p_content), label(p_label)
{
}

void TextLabelled::_Draw_Impl()
{
    ImGui::LabelText((label + m_widgetID).c_str(), content.c_str());
}
} // namespace NLS::UI::Widgets
