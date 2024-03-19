#pragma once
#include "CollisionVolume.h"
#include "Math/Vector3.h"
#include "EngineDef.h"
namespace NLS
{
class NLS_ENGINE_API OBBVolume : public CollisionVolume
{
public:
    OBBVolume()
    {
        type = VolumeType::OBB;
    }
    ~OBBVolume() {}

    Maths::Vector3 GetHalfDimensions() const
    {
        return halfSizes;
    }
    void SetHalfDimensions(const  Maths::Vector3& halfDims)
    {
        halfSizes = halfDims;
    }

protected:
    Maths::Vector3 halfSizes;
};
} // namespace NLS
