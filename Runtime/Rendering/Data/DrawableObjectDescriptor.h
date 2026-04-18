#pragma once

#include <Math/Matrix4.h>

namespace NLS::Render::Data
{
    struct DrawableObjectDescriptor
    {
        Maths::Matrix4 modelMatrix = Maths::Matrix4::Identity;
        Maths::Matrix4 userMatrix = Maths::Matrix4::Identity;
    };
}
