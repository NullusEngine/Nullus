#pragma once

#include <cstdint>

#include "MathDef.h"

namespace NLS::Maths
{
struct NLS_MATH_API Vector3Int
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    constexpr Vector3Int() = default;
    constexpr Vector3Int(const int32_t xValue, const int32_t yValue, const int32_t zValue)
        : x(xValue), y(yValue), z(zValue)
    {
    }

    friend constexpr bool operator==(const Vector3Int&, const Vector3Int&) = default;
};
} // namespace NLS::Maths
