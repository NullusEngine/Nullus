#pragma once

#include <cmath>

#include <Math/Vector3.h>
#include <Math/Transform.h>

#include "Rendering/Entities/Entity.h"
#include "Rendering/Settings/ELightType.h"
#include "RenderDef.h"
namespace NLS::Render::Entities
{
/**
 * Data structure that can represent any type of light
 */
class NLS_RENDER_API Light : public Entity
{
public :
    Light(Maths::Transform* p_transform)
        : Entity{p_transform}
    {
    }
    Maths::Vector3 color{1.f, 1.f, 1.f};
    float intensity = 1.f;
    float range = 10.0f;
    Maths::Vector3 size{1.0f, 1.0f, 1.0f};
    float cutoff = 12.f;
    float outerCutoff = 15.f;
    Settings::ELightType type = Settings::ELightType::POINT;

    /** Returns a finite, nonnegative range for rendering consumers. */
    float GetSafeRange() const
    {
        return std::isfinite(range) && range > 0.0f ? range : 0.0f;
    }
};
} // namespace NLS::Render::Entities
