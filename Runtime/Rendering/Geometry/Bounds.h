#pragma once

#include <Math/Vector3.h>

#include "Rendering/RenderDef.h"

namespace NLS::Render::Geometry
{
struct NLS_RENDER_API Bounds
{
    NLS::Maths::Vector3 center {};
    NLS::Maths::Vector3 size {};
};
} // namespace NLS::Render::Geometry
