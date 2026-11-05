#pragma once

#include "PhysicsDef.h"
#include <Math/Vector3.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace NLS::Physics
{
enum class CollisionMeshKind : uint32_t { Convex, Triangle, HeightField };

struct CollisionMeshSource
{
    CollisionMeshKind kind = CollisionMeshKind::Convex;
    std::vector<Maths::Vector3> vertices;
    std::vector<uint32_t> indices;
    std::vector<float> heights;
    uint32_t rows = 0, columns = 0;
    float rowSpacing = 1, columnSpacing = 1;
    Maths::Vector3 bakeScale{1,1,1};
    bool weldVertices = false;
    float weldTolerance = 0.001f;
    uint32_t convexVertexLimit = 255;
};

// Immutable cooked data, independent of any world. Backend objects remain private.
// Cooking is an authoring operation; Deserialize and runtime attachment never cook.
class NLS_PHYSICS_API CookedCollisionMesh final
{
public:
    static std::shared_ptr<const CookedCollisionMesh> Cook(const CollisionMeshSource& source, std::string& error);
    static std::shared_ptr<const CookedCollisionMesh> Deserialize(std::span<const uint8_t> bytes, std::string& error);
    static std::string CompatibilityTag();
    std::vector<uint8_t> Serialize() const;
    CollisionMeshKind GetKind() const;
    Maths::Vector3 GetBakedScale() const;
    size_t GetCookedByteCount() const;
    ~CookedCollisionMesh();

private:
    struct Impl;
    explicit CookedCollisionMesh(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;
    friend class PhysicsWorld;
};
}
