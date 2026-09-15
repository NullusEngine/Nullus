#pragma once

#include <cstdint>

#include "MathDef.h"

namespace NLS::Maths
{
struct NLS_MATH_API Vector2Int
{
    int32_t x = 0;
    int32_t y = 0;

    constexpr Vector2Int() = default;
    constexpr Vector2Int(const int32_t xValue, const int32_t yValue)
        : x(xValue), y(yValue)
    {
    }

    friend constexpr bool operator==(const Vector2Int&, const Vector2Int&) = default;
};
} // namespace NLS::Maths
