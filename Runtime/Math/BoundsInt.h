#pragma once

#include "MathDef.h"
#include "Vector3Int.h"

namespace NLS::Maths
{
struct NLS_MATH_API BoundsInt
{
    Vector3Int position {};
    Vector3Int size {};

    friend constexpr bool operator==(const BoundsInt&, const BoundsInt&) = default;
};
} // namespace NLS::Maths
