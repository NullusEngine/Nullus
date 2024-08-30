#include "UI/Widgets/Texts/TextDisabled.h"

namespace NLS::UI::Widgets
{
TextDisabled::TextDisabled(const std::string& p_content)
    : Text(p_content)
{
}

void TextDisabled::_Draw_Impl()
{
    ImGui::TextDisabled(content.c_str());
}
} // namespace NLS::UI::Widgets
