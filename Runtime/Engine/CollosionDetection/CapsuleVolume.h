#pragma once
#include "CollisionVolume.h"
#include "EngineDef.h"
namespace NLS
{
class NLS_ENGINE_API CapsuleVolume : public CollisionVolume
{
public:
    CapsuleVolume(float halfHeight, float radius)
    {
        this->halfHeight = halfHeight;
        this->radius = radius;
        this->type = VolumeType::Capsule;
    };
    ~CapsuleVolume()
    {
    }
    float GetRadius() const
    {
        return radius;
    }

    float GetHalfHeight() const
    {
        return halfHeight;
    }

protected:
    float radius;
    float halfHeight;
};
} // namespace NLS
