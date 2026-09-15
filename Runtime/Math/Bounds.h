#pragma once

#include "MathDef.h"
#include "Vector3.h"

namespace NLS::Maths
{
struct NLS_MATH_API Bounds
{
    Vector3 center {};
    Vector3 size {};

    friend bool operator==(const Bounds& left, const Bounds& right)
    {
        return left.center.x == right.center.x && left.center.y == right.center.y && left.center.z == right.center.z &&
            left.size.x == right.size.x && left.size.y == right.size.y && left.size.z == right.size.z;
    }
};
} // namespace NLS::Maths
