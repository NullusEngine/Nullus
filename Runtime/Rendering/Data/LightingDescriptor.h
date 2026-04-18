#pragma once

#include <functional>
#include <vector>

#include "Rendering/Entities/Light.h"
#include "RenderDef.h"

namespace NLS::Render::Data
{
    using LightSet = std::vector<std::reference_wrapper<const NLS::Render::Entities::Light>>;

    struct NLS_RENDER_API LightingDescriptor
    {
        LightSet lights;
    };
}
