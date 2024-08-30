#include "UI/Widgets/Visual/ProgressBar.h"
#include "UI/Internal/Converter.h"

namespace NLS::UI::Widgets
{
ProgressBar::ProgressBar(float p_fraction, const Maths::Vector2& p_size, const std::string& p_overlay)
    : fraction(p_fraction), size(p_size), overlay(p_overlay)
{
}

void ProgressBar::_Draw_Impl()
{
    ImGui::ProgressBar(fraction, Internal::Converter::ToImVec2(size), !overlay.empty() ? overlay.c_str() : nullptr);
}
} // namespace NLS
