#include "Color.h"

namespace NLS::Maths
{
const Color Color::Red = {1.f, 0.f, 0.f};
const Color Color::Green = {0.f, 1.f, 0.f};
const Color Color::Blue = {0.f, 0.f, 1.f};
const Color Color::White = {1.f, 1.f, 1.f};
const Color Color::Black = {0.f, 0.f, 0.f};
const Color Color::Grey = {0.5f, 0.5f, 0.5f};
const Color Color::Yellow = {1.f, 1.f, 0.f};
const Color Color::Cyan = {0.f, 1.f, 1.f};
const Color Color::Magenta = {1.f, 0.f, 1.f};

Color::Color(float p_r, float p_g, float p_b, float p_a)
    : r(p_r), g(p_g), b(p_b), a(p_a)
{
}

Color::Color(Maths::Vector3 p_vector)
    : Color(p_vector.x, p_vector.y, p_vector.z)
{
}

Color::Color(Maths::Vector4 p_vector)
    : Color(p_vector.x, p_vector.y, p_vector.z, p_vector.w)
{
}

bool Color::operator==(const Color& p_other)
{
    return this->r == p_other.r && this->g == p_other.g && this->b == p_other.b && this->a == p_other.a;
}

bool Color::operator!=(const Color& p_other)
{
    return !operator==(p_other);
}

} // namespace NLS
