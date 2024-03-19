#pragma once
#include "CollisionVolume.h"
#include "Math/Vector3.h"
#include "EngineDef.h"
namespace NLS
{
class NLS_ENGINE_API AABBVolume : public CollisionVolume
{
public:
    AABBVolume()
    {
        type = VolumeType::AABB;
    }
    ~AABBVolume()
    {
    }

    Vector3 GetHalfDimensions() const
    {
        return halfSizes;
    }
    void SetHalfDimensions(const Vector3& halfDims)
    {
        halfSizes = halfDims;
    }

protected:
    Vector3 halfSizes;
};
} // namespace NLS
