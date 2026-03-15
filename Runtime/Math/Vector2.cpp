#include "Vector4.h"
#include "Vector3.h"
#include "Vector2.h"
using namespace NLS;
using namespace Maths;

const Vector2 Vector2::One(1.0f, 1.0f);
const Vector2 Vector2::Zero(0.0f, 0.0f);
Vector2::Vector2(const Vector3& v3)
    : x(v3.x), y(v3.y)
{
}

Vector2::Vector2(const Vector4& v4)
    : x(v4.x), y(v4.y)
{
}

Vector2::Vector2(const Vector2& p_toCopy)
    : x(p_toCopy.x), y(p_toCopy.y)
{
}

NLS::Maths::Vector2 Vector2::operator=(const Vector2& p_other)
{
    this->x = p_other.x;
    this->y = p_other.y;

    return *this;
}

Vector2 Vector2::Add(const Vector2& p_left, const Vector2& p_right)
{
    return Vector2(
        p_left.x + p_right.x,
        p_left.y + p_right.y);
}

Vector2 Vector2::Substract(const Vector2& p_left, const Vector2& p_right)
{
    return Vector2(
        p_left.x - p_right.x,
        p_left.y - p_right.y);
}

Vector2 Vector2::Multiply(const Vector2& p_target, float p_scalar)
{
    return Vector2(
        p_target.x * p_scalar,
        p_target.y * p_scalar);
}

Vector2 Vector2::Divide(const Vector2& p_left, float p_scalar)
{
    Vector2 result(p_left);

    if (p_scalar == 0)
        throw std::logic_error("Division by 0");

    result.x /= p_scalar;
    result.y /= p_scalar;

    return result;
}

float Vector2::Length(const Vector2& p_target)
{
    return sqrtf(p_target.x * p_target.x + p_target.y * p_target.y);
}

Vector2 Vector2::Normalize(const Vector2& p_target)
{
    float length = Length(p_target);

    if (length > 0.0f)
    {
        float targetLength = 1.0f / length;

        return Vector2(
            p_target.x * targetLength,
            p_target.y * targetLength);
    }
    else
    {
        return Vector2::Zero;
    }
}

Vector2 Vector2::Lerp(const Vector2& p_start, const Vector2& p_end, float p_alpha)
{
    return (p_start + (p_end - p_start) * p_alpha);
}

float Vector2::AngleBetween(const Vector2& p_from, const Vector2& p_to)
{
    float lengthProduct = Length(p_from) * Length(p_to);

    if (lengthProduct > 0.0f)
    {
        float fractionResult = Dot(p_from, p_to) / lengthProduct;

        if (fractionResult >= -1.0f && fractionResult <= 1.0f)
            return acosf(fractionResult);
    }

    return 0.0f;
}
