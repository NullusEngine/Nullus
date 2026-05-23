#pragma once

#include "RenderDef.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Rendering/Geometry/Vertex.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace NLS::Render::Geometry
{
inline BoundingSphere ComputeBoundingSphere(const std::vector<Vertex>& vertices)
{
    BoundingSphere boundingSphere{};
    boundingSphere.position = Maths::Vector3::Zero;
    boundingSphere.radius = 0.0f;

    if (vertices.empty())
        return boundingSphere;

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float minZ = std::numeric_limits<float>::max();

    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    float maxZ = std::numeric_limits<float>::lowest();

    for (const auto& vertex : vertices)
    {
        minX = std::min(minX, vertex.position[0]);
        minY = std::min(minY, vertex.position[1]);
        minZ = std::min(minZ, vertex.position[2]);

        maxX = std::max(maxX, vertex.position[0]);
        maxY = std::max(maxY, vertex.position[1]);
        maxZ = std::max(maxZ, vertex.position[2]);
    }

    boundingSphere.position = Maths::Vector3{ minX + maxX, minY + maxY, minZ + maxZ } / 2.0f;

    for (const auto& vertex : vertices)
    {
        const Maths::Vector3 position{
            vertex.position[0],
            vertex.position[1],
            vertex.position[2]
        };
        boundingSphere.radius = std::max(
            boundingSphere.radius,
            Maths::Vector3::Distance(boundingSphere.position, position));
    }

    return boundingSphere;
}
}
