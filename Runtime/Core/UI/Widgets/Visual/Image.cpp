#include "UI/Widgets/Visual/Image.h"
#include "UI/Internal/Converter.h"

namespace NLS
{
UI::Widgets::Visual::Image::Image(uint32_t p_textureID, const Maths::Vector2& p_size)
    : textureID{p_textureID}, size(p_size)
{
}

void UI::Widgets::Visual::Image::_Draw_Impl()
{
    ImGui::Image(textureID.raw, Internal::Converter::ToImVec2(size), ImVec2(0.f, 1.f), ImVec2(1.f, 0.f));
}
} // namespace NLS
