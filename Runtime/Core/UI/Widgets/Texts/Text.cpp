#include "UI/Widgets/Texts/Text.h"

namespace NLS
{
UI::Widgets::Texts::Text::Text(const std::string& p_content /*= ""*/, float p_scale)
    : DataWidget(content), content(p_content), m_scale(p_scale)
{
}

void UI::Widgets::Texts::Text::_Draw_Impl()
{
    ImGui::SetWindowFontScale(m_scale); 
    ImGui::Text(content.c_str());
    ImGui::SetWindowFontScale(1.f);
}
} // namespace NLS
