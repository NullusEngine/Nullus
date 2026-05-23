#pragma once

#include "EngineDef.h"

namespace NLS::Engine
{
    enum class PrimitiveType
    {
        Cube,
        Sphere,
        Cone,
        Cylinder,
        Plane,
        Gear,
        Helix,
        Pipe,
        Pyramid,
        Torus
    };

    NLS_ENGINE_API const char* GetPrimitiveName(PrimitiveType type);
}
