#pragma once

#include <cstdint>

#include "MathDef.h"

namespace NLS::Maths
{
struct NLS_MATH_API RectInt
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    constexpr RectInt() = default;
    constexpr RectInt(
        const int32_t xValue,
        const int32_t yValue,
        const int32_t widthValue,
        const int32_t heightValue)
        : x(xValue), y(yValue), width(widthValue), height(heightValue)
    {
    }

    friend constexpr bool operator==(const RectInt&, const RectInt&) = default;
};
} // namespace NLS::Maths
