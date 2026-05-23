#pragma once

#include "MathDef.h"

namespace NLS::Maths
{
class NLS_MATH_API Rect
{
public:
    constexpr Rect(float p_x = 0.0f, float p_y = 0.0f, float p_width = 0.0f, float p_height = 0.0f)
        : x(p_x), y(p_y), width(p_width), height(p_height)
    {
    }

    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};
} // namespace NLS::Maths
