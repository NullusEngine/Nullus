#pragma once
#include "CollisionVolume.h"
#include "EngineDef.h"
namespace NLS
{
class NLS_ENGINE_API CapsuleVolume : public CollisionVolume
{
public:
    CapsuleVolume()
    {
        this->type = VolumeType::Capsule;
    };
    ~CapsuleVolume()
    {
    }
    float GetRadius() const
    {
        return radius;
    }

    void SetRadius(float radius)
    {
        this->radius = radius;
    }

    float GetHalfHeight() const
    {
        return halfHeight;
    }

    void SetHalfHeight(float halfHeight)
    {
        this->halfHeight = halfHeight;
    }

protected:
    float radius;
    float halfHeight;
};
} // namespace NLS
