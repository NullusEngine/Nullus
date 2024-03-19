#pragma once
#include "CollisionVolume.h"
#include "EngineDef.h"
namespace NLS
{
class NLS_ENGINE_API SphereVolume : public CollisionVolume
{
public:
    SphereVolume()
    {
        type = VolumeType::Sphere;
    }
    ~SphereVolume() {}

    float GetRadius() const
    {
        return radius;
    }
    void SetRadius(float radius)
    {
        this->radius = radius;
    }

protected:
    float radius;
};
} // namespace NLS
