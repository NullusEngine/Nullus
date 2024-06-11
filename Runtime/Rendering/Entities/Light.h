#pragma once

#include <Math/Vector3.h>
#include <Math/Matrix4.h>
#include <Math/Transform.h>

#include "Rendering/Entities/Entity.h"
#include "Rendering/Settings/ELightType.h"
#include "RenderDef.h"
using namespace NLS;
namespace NLS::Rendering::Entities
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
    float constant = 0.0f;
    float linear = 0.0f;
    float quadratic = 1.0f;
    float cutoff = 12.f;
    float outerCutoff = 15.f;
    Settings::ELightType type = Settings::ELightType::POINT;

    /**
     * Generate the light matrix, ready to send to the GPU
     */
    Maths::Matrix4 GenerateMatrix() const;

    /**
     * Calculate the light effect range from the quadratic falloff equation
     */
    float GetEffectRange() const;
};
} // namespace NLS::Rendering::Entities