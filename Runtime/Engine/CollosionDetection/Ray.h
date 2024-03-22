#pragma once
#include "Math/Vector3.h"
#include "Math/Plane.h"
#include "EngineDef.h"
namespace NLS
{
namespace Maths
{
struct NLS_ENGINE_API RayCollision
{
    void* node;         // Node that was hit
    Vector3 collidedAt; // WORLD SPACE position of the collision!
    float rayDistance;

    RayCollision()
    {
        node = nullptr;
        rayDistance = MAX_FLOAT;
    }
};

class NLS_ENGINE_API Ray
{
public:
    Ray(Vector3 position, Vector3 direction)
    {
        this->position = position;
        this->direction = direction;
    }
    ~Ray(void) {}

    Vector3 GetPosition() const { return position; }

    Vector3 GetDirection() const { return direction; }

protected:
    Vector3 position;  // World space position
    Vector3 direction; // Normalised world space direction
};
} // namespace Maths
} // namespace NLS