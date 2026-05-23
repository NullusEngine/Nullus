#pragma once

#include <algorithm>
#include <cstdint>

#include "Rendering/Entities/Drawable.h"

namespace NLS::Render::Data
{
    struct DrawableInstanceCount
    {
        uint32_t count = 0u;
        bool fromDrawable = false;
        bool fromMaterial = false;
    };

    inline DrawableInstanceCount ResolveDrawableInstanceCount(const Entities::Drawable& drawable)
    {
        if (drawable.instanceCount != 0u)
            return { drawable.instanceCount, true, false };

        if (drawable.material == nullptr)
            return {};

        const auto materialInstances = drawable.material->GetGPUInstances();
        return {
            static_cast<uint32_t>(std::max(materialInstances, 0)),
            false,
            materialInstances > 0
        };
    }
}
