#include "UI/Widgets/Texts/TextWrapped.h"

namespace NLS::UI::Widgets
{
TextWrapped::TextWrapped(const std::string& p_content)
    : Text(p_content)
{
}

void TextWrapped::_Draw_Impl()
{
    ImGui::TextWrapped(content.c_str());
}
} // namespace NLS::UI::Widgets
