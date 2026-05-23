#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include <Math/Matrix4.h>

namespace NLS::Render::Data
{
    struct DrawableObjectDescriptor
    {
        static constexpr uint32_t kInvalidObjectIndex = (std::numeric_limits<uint32_t>::max)();

        Maths::Matrix4 modelMatrix = Maths::Matrix4::Identity;
        Maths::Matrix4 userMatrix = Maths::Matrix4::Identity;
        uint32_t objectIndex = kInvalidObjectIndex;
        uint32_t objectCount = 1u;
        std::vector<Maths::Matrix4> instanceModelMatrices;
    };
}
