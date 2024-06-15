#include "UI/Widgets/Texts/TextWrapped.h"

namespace NLS
{
UI::Widgets::Texts::TextWrapped::TextWrapped(const std::string& p_content)
    : Text(p_content)
{
}

void UI::Widgets::Texts::TextWrapped::_Draw_Impl()
{
    ImGui::TextWrapped(content.c_str());
}
} // namespace NLS
