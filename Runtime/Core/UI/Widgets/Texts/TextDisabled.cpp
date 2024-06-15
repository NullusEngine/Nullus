#include "UI/Widgets/Texts/TextDisabled.h"

namespace NLS
{
UI::Widgets::Texts::TextDisabled::TextDisabled(const std::string& p_content)
    : Text(p_content)
{
}

void UI::Widgets::Texts::TextDisabled::_Draw_Impl()
{
    ImGui::TextDisabled(content.c_str());
}
} // namespace NLS
