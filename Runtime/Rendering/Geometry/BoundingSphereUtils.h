#pragma once

#include "Math/Matrix4.h"
#include "Math/Vector4.h"
#include "RenderDef.h"
#include "Rendering/Geometry/Bounds.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Rendering/Geometry/Vertex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace NLS::Render::Geometry
{
inline Bounds ComputeBounds(const std::vector<Vertex>& vertices)
{
    Bounds bounds{};
    bounds.center = Maths::Vector3::Zero;
    bounds.size = Maths::Vector3::Zero;

    if (vertices.empty())
        return bounds;

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

    bounds.center = Maths::Vector3{ minX + maxX, minY + maxY, minZ + maxZ } / 2.0f;
    bounds.size = Maths::Vector3{ maxX - minX, maxY - minY, maxZ - minZ };
    return bounds;
}

inline Bounds UnionBounds(const Bounds& lhs, const Bounds& rhs)
{
    const auto lhsHalfSize = lhs.size * 0.5f;
    const auto rhsHalfSize = rhs.size * 0.5f;
    const auto minPoint = Maths::Vector3{
        std::min(lhs.center.x - lhsHalfSize.x, rhs.center.x - rhsHalfSize.x),
        std::min(lhs.center.y - lhsHalfSize.y, rhs.center.y - rhsHalfSize.y),
        std::min(lhs.center.z - lhsHalfSize.z, rhs.center.z - rhsHalfSize.z)
    };
    const auto maxPoint = Maths::Vector3{
        std::max(lhs.center.x + lhsHalfSize.x, rhs.center.x + rhsHalfSize.x),
        std::max(lhs.center.y + lhsHalfSize.y, rhs.center.y + rhsHalfSize.y),
        std::max(lhs.center.z + lhsHalfSize.z, rhs.center.z + rhsHalfSize.z)
    };

    Bounds bounds;
    bounds.center = (minPoint + maxPoint) * 0.5f;
    bounds.size = maxPoint - minPoint;
    return bounds;
}

inline std::array<Maths::Vector3, 8u> BuildBoundsCorners(const Bounds& bounds)
{
    const auto halfSize = bounds.size * 0.5f;
    return {
        bounds.center + Maths::Vector3{ -halfSize.x, -halfSize.y, -halfSize.z },
        bounds.center + Maths::Vector3{ -halfSize.x, -halfSize.y, halfSize.z },
        bounds.center + Maths::Vector3{ -halfSize.x, halfSize.y, -halfSize.z },
        bounds.center + Maths::Vector3{ -halfSize.x, halfSize.y, halfSize.z },
        bounds.center + Maths::Vector3{ halfSize.x, -halfSize.y, -halfSize.z },
        bounds.center + Maths::Vector3{ halfSize.x, -halfSize.y, halfSize.z },
        bounds.center + Maths::Vector3{ halfSize.x, halfSize.y, -halfSize.z },
        bounds.center + Maths::Vector3{ halfSize.x, halfSize.y, halfSize.z }
    };
}

inline Maths::Vector3 TransformPoint(const Maths::Matrix4& matrix, const Maths::Vector3& point)
{
    const auto transformed = matrix * Maths::Vector4(point, 1.0f);
    if (transformed.w != 0.0f && transformed.w != 1.0f)
    {
        return {
            transformed.x / transformed.w,
            transformed.y / transformed.w,
            transformed.z / transformed.w
        };
    }

    return { transformed.x, transformed.y, transformed.z };
}

inline Bounds TransformBounds(const Bounds& bounds, const Maths::Matrix4& matrix)
{
    const auto corners = BuildBoundsCorners(bounds);
    Maths::Vector3 minPoint {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    Maths::Vector3 maxPoint {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    };

    for (const auto& corner : corners)
    {
        const auto point = TransformPoint(matrix, corner);
        minPoint.x = std::min(minPoint.x, point.x);
        minPoint.y = std::min(minPoint.y, point.y);
        minPoint.z = std::min(minPoint.z, point.z);
        maxPoint.x = std::max(maxPoint.x, point.x);
        maxPoint.y = std::max(maxPoint.y, point.y);
        maxPoint.z = std::max(maxPoint.z, point.z);
    }

    Bounds transformedBounds;
    transformedBounds.center = (minPoint + maxPoint) * 0.5f;
    transformedBounds.size = maxPoint - minPoint;
    return transformedBounds;
}

inline bool BoundsOverlap(const Bounds& lhs, const Bounds& rhs)
{
    const auto lhsHalfSize = lhs.size * 0.5f;
    const auto rhsHalfSize = rhs.size * 0.5f;
    return std::abs(lhs.center.x - rhs.center.x) <= lhsHalfSize.x + rhsHalfSize.x &&
        std::abs(lhs.center.y - rhs.center.y) <= lhsHalfSize.y + rhsHalfSize.y &&
        std::abs(lhs.center.z - rhs.center.z) <= lhsHalfSize.z + rhsHalfSize.z;
}

inline BoundingSphere ComputeBoundingSphere(const std::vector<Vertex>& vertices)
{
    BoundingSphere boundingSphere{};
    boundingSphere.position = Maths::Vector3::Zero;
    boundingSphere.radius = 0.0f;

    if (vertices.empty())
        return boundingSphere;

    const auto bounds = ComputeBounds(vertices);
    boundingSphere.position = bounds.center;

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
