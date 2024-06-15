#include "UI/Widgets/Texts/Text.h"

namespace NLS
{
UI::Widgets::Texts::Text::Text(const std::string& p_content)
    : DataWidget(content), content(p_content)
{
}

void UI::Widgets::Texts::Text::_Draw_Impl()
{
    ImGui::Text(content.c_str());
}
} // namespace NLS
