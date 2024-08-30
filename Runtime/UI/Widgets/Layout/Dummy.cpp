#include "UI/Widgets/Layout/Dummy.h"
#include "UI/Internal/Converter.h"

namespace NLS::UI::Widgets
{
Dummy::Dummy(const Maths::Vector2& p_size)
    : size(p_size)
{
}

void Dummy::_Draw_Impl()
{
    ImGui::Dummy(Internal::Converter::ToImVec2(size));
}
} // namespace NLS
