
#include "UI/Widgets/Texts/TextColored.h"
#include "UI/Internal/Converter.h"

namespace NLS::UI::Widgets
{
TextColored::TextColored(const std::string& p_content, const Maths::Color& p_color)
    : Text(p_content), color(p_color)
{
}

void TextColored::_Draw_Impl()
{
    ImGui::TextColored(Internal::Converter::ToImVec4(color), content.c_str());
}
} // namespace NLS::UI::Widgets
