
#include "UI/Internal/Converter.h"
namespace NLS
{
    ImVec4 NLS::UI::Internal::Converter::ToImVec4(const Types::Color& p_value)
    {
        return ImVec4(p_value.r, p_value.g, p_value.b, p_value.a);
    }

    NLS::UI::Types::Color NLS::UI::Internal::Converter::ToColor(const ImVec4& p_value)
    {
        return Types::Color(p_value.x, p_value.y, p_value.z, p_value.w);
    }

    ImVec2 NLS::UI::Internal::Converter::ToImVec2(const Maths::Vector2& p_value)
    {
        return ImVec2(p_value.x, p_value.y);
    }

    Maths::Vector2 NLS::UI::Internal::Converter::ToFVector2(const ImVec2& p_value)
    {
        return Maths::Vector2(p_value.x, p_value.y);
    }
}

