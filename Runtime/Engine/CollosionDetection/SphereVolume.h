#pragma once
#include "CollisionVolume.h"
#include "EngineDef.h"
namespace NLS
{
class NLS_ENGINE_API SphereVolume : CollisionVolume
{
public:
    SphereVolume(float sphereRadius = 1.0f)
    {
        type = VolumeType::Sphere;
        radius = sphereRadius;
    }
    ~SphereVolume() {}

    float GetRadius() const
    {
        return radius;
    }

protected:
    float radius;
};
} // namespace NLS
