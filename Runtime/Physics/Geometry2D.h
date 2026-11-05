#pragma once

#include "PhysicsCommandBuffer.h"

namespace NLS::Physics
{
inline constexpr size_t MaxPolygonVertices2D = 256;
inline constexpr float MinEdgeLength2D = 0.01f;
inline constexpr float MaxGeometryCoordinate2D = 1e6f;

struct ChainGeometrySettings2D
{
    bool loop = false;
    bool hasPreviousGhost = false, hasNextGhost = false;
    Maths::Vector2 previousGhost, nextGhost;
};

// Pure, bounded builders. A failure leaves output unchanged. Polygon input is a
// simple contour without holes or a repeated closing vertex; either winding is
// accepted. Chain fronts point right along each edge. Part IDs start at one and
// identify this geometry revision's decomposition/segment order.
NLS_PHYSICS_API bool BuildPolygon2D(std::span<const Maths::Vector2> vertices,const ShapeSettings2D& settings,
    std::vector<PhysicsShapeDescription2D>& output,std::string& error);
NLS_PHYSICS_API bool BuildChain2D(std::span<const Maths::Vector2> vertices,const ChainGeometrySettings2D& chain,
    const ShapeSettings2D& settings,std::vector<PhysicsShapeDescription2D>& output,std::string& error);

namespace Detail
{
bool ValidGeometryPoint2D(Maths::Vector2 point);
bool ValidGeometryEdge2D(Maths::Vector2 first,Maths::Vector2 second);
}
}
