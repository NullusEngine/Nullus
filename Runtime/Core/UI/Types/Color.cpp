#include "UI/Types/Color.h"

namespace NLS
{
const UI::Types::Color UI::Types::Color::Red = {1.f, 0.f, 0.f};
const UI::Types::Color UI::Types::Color::Green = {0.f, 1.f, 0.f};
const UI::Types::Color UI::Types::Color::Blue = {0.f, 0.f, 1.f};
const UI::Types::Color UI::Types::Color::White = {1.f, 1.f, 1.f};
const UI::Types::Color UI::Types::Color::Black = {0.f, 0.f, 0.f};
const UI::Types::Color UI::Types::Color::Grey = {0.5f, 0.5f, 0.5f};
const UI::Types::Color UI::Types::Color::Yellow = {1.f, 1.f, 0.f};
const UI::Types::Color UI::Types::Color::Cyan = {0.f, 1.f, 1.f};
const UI::Types::Color UI::Types::Color::Magenta = {1.f, 0.f, 1.f};

UI::Types::Color::Color(float p_r, float p_g, float p_b, float p_a)
    : r(p_r), g(p_g), b(p_b), a(p_a)
{
}

UI::Types::Color::Color(Maths::Vector3 p_vector)
    : Color(p_vector.x, p_vector.y, p_vector.z)
{
}

UI::Types::Color::Color(Maths::Vector4 p_vector)
    : Color(p_vector.x, p_vector.y, p_vector.z, p_vector.w)
{
}

bool UI::Types::Color::operator==(const Color& p_other)
{
    return this->r == p_other.r && this->g == p_other.g && this->b == p_other.b && this->a == p_other.a;
}

bool UI::Types::Color::operator!=(const Color& p_other)
{
    return !operator==(p_other);
}

} // namespace NLS
