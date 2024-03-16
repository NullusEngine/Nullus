#pragma once
#include "CollisionVolume.h"
#include "Math/Vector3.h"
#include "EngineDef.h"
namespace NLS
{
class NLS_ENGINE_API AABBVolume : CollisionVolume
{
public:
    AABBVolume(const Vector3& halfDims)
    {
        type = VolumeType::AABB;
        halfSizes = halfDims;
    }
    ~AABBVolume()
    {
    }

    Vector3 GetHalfDimensions() const
    {
        return halfSizes;
    }

protected:
    Vector3 halfSizes;
};
} // namespace NLS
