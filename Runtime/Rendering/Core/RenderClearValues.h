#pragma once

#include <Math/Vector4.h>

namespace NLS::Render::Core
{
    inline NLS::Maths::Vector4 DefaultOpaqueClearColor()
    {
        return { 0.0f, 0.0f, 0.0f, 1.0f };
    }
}
