#pragma once
#include "CollisionVolume.h"
#include "Math/Vector3.h"
#include "EngineDef.h"
namespace NLS
{
class NLS_ENGINE_API OBBVolume : CollisionVolume
{
public:
    OBBVolume(const Maths::Vector3& halfDims)
    {
        type = VolumeType::OBB;
        halfSizes = halfDims;
    }
    ~OBBVolume() {}

    Maths::Vector3 GetHalfDimensions() const
    {
        return halfSizes;
    }

protected:
    Maths::Vector3 halfSizes;
};
} // namespace NLS
