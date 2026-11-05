#pragma once

#include <Math/Vector2.h>
#include <Math/Vector3.h>
#include <cmath>
#include <cstdint>
#include <string>

namespace NLS::Engine::SceneSystem
{
// Authoring data only. Fixed-step scheduling belongs to RuntimePlaySession.
struct ScenePhysicsSettings
{
    Maths::Vector3 gravity3D{0, -9.81f, 0};
    Maths::Vector2 gravity2D{0, -9.81f};
    uint32_t solverSubsteps2D = 4;
    uint32_t workerCount3D = 0, workerCount2D = 0;
    bool interpolate = false;

    bool operator==(const ScenePhysicsSettings&) const = default;
    bool Validate(std::string& error) const
    {
        if (!std::isfinite(gravity3D.x) || !std::isfinite(gravity3D.y) || !std::isfinite(gravity3D.z) ||
            !std::isfinite(gravity2D.x) || !std::isfinite(gravity2D.y))
        { error = "Scene physics gravity must be finite."; return false; }
        if (solverSubsteps2D < 1 || solverSubsteps2D > 64)
        { error = "Scene Box2D solver subdivisions must be between 1 and 64."; return false; }
        if (workerCount3D > 31 || workerCount2D > 31)
        { error = "Scene physics worker counts must be between 0 and 31."; return false; }
        error.clear(); return true;
    }
};
}
