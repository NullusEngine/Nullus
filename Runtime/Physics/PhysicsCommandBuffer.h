#pragma once

#include "PhysicsWorld.h"
#include "PhysicsWorld2D.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace NLS::Physics
{
struct PhysicsCommandTicket
{
    uint64_t buffer = 0, generation = 0;
    size_t index = ~size_t{0};
    bool IsValid() const { return buffer != 0 && generation != 0 && index != ~size_t{0}; }
    bool operator==(const PhysicsCommandTicket&) const = default;
};

struct PhysicsBodyReference
{
    BodyHandle body;
    BodyHandle2D body2D;
    PhysicsCommandTicket command;
    PhysicsBodyReference() = default;
    PhysicsBodyReference(BodyHandle value) : body(value) {}
    PhysicsBodyReference(BodyHandle2D value) : body2D(value) {}
    PhysicsBodyReference(PhysicsCommandTicket value) : command(value) {}
};

struct PhysicsShapeReference
{
    ShapeHandle shape;
    ShapeHandle2D shape2D;
    PhysicsCommandTicket command;
    PhysicsShapeReference() = default;
    PhysicsShapeReference(ShapeHandle value) : shape(value) {}
    PhysicsShapeReference(ShapeHandle2D value) : shape2D(value) {}
    PhysicsShapeReference(PhysicsCommandTicket value) : command(value) {}
};

enum class CollisionGeometryType3D : uint8_t { Box, Sphere, Capsule, Cylinder, ConvexMesh, TriangleMesh, HeightField, CookedMesh };
struct CollisionGeometry3D
{
    CollisionGeometryType3D type = CollisionGeometryType3D::Box;
    Maths::Vector3 halfExtents{0.5f, 0.5f, 0.5f};
    float radius = 0.5f, halfHeight = 0.5f;
    std::vector<Maths::Vector3> vertices;
    std::vector<uint32_t> indices;
    uint32_t rows = 0, columns = 0;
    std::vector<float> heights;
    float rowSpacing = 1, columnSpacing = 1;
    std::shared_ptr<const CookedCollisionMesh> cookedMesh;
    Maths::Vector3 meshScale{1,1,1};
};

enum class CollisionGeometryType2D : uint8_t { Box, Circle, Capsule, ConvexPolygon, Segment, ChainSegment };
struct CollisionGeometry2D
{
    CollisionGeometryType2D type = CollisionGeometryType2D::Box;
    Maths::Vector2 halfExtents{0.5f, 0.5f};
    float radius = 0.5f, halfHeight = 0.5f;
    std::vector<Maths::Vector2> vertices;
};

inline constexpr size_t MaxCompoundShapes3D = 256;
inline constexpr size_t MaxCompoundShapes2D = 256;
struct PhysicsShapeDescription2D
{
    CollisionGeometry2D geometry;
    ShapeSettings2D settings;
};
struct PhysicsShapeDescription3D
{
    CollisionGeometry3D geometry;
    ShapeSettings settings;
};

enum class PhysicsCommandStatus : uint8_t { Invalid, Pending, Applied, Skipped, Failed };
enum class PhysicsCommandError : uint8_t
{
    None, InvalidReference, ExpiredWorld, WrongDimension, BackendRejected, StaleOrForeignHandle, UnexpectedException
};
struct PhysicsCommandResult
{
    PhysicsCommandStatus status = PhysicsCommandStatus::Invalid;
    PhysicsCommandError error = PhysicsCommandError::None;
    BodyHandle body;
    ShapeHandle shape;
    BodyHandle2D body2D;
    ShapeHandle2D shape2D;
    std::vector<ShapeHandle> shapes;
    std::vector<ShapeHandle2D> shapes2D;
};
struct PhysicsCommandCommitReport
{
    size_t processed = 0, applied = 0, skipped = 0, failed = 0, pending = 0;
    bool busy = false;
};

// One world per buffer. Producers only record owned value snapshots; Commit
// executes on the world's owning thread, outside callbacks and retained reads.
// A commit publishes one mutation batch, preserves FIFO and never runs scripts.
class NLS_PHYSICS_API PhysicsCommandBuffer final
{
public:
    explicit PhysicsCommandBuffer(const std::shared_ptr<PhysicsWorld>& world);
    explicit PhysicsCommandBuffer(const std::shared_ptr<PhysicsWorld2D>& world);
    ~PhysicsCommandBuffer();
    PhysicsCommandBuffer(const PhysicsCommandBuffer&) = delete;
    PhysicsCommandBuffer& operator=(const PhysicsCommandBuffer&) = delete;

    bool Matches(const std::shared_ptr<PhysicsWorld>& world) const;
    bool Matches(const std::shared_ptr<PhysicsWorld2D>& world) const;
    PhysicsCommandTicket CreateBody(const BodySettings& settings = {});
    PhysicsCommandTicket CreateBody(const BodySettings2D& settings);
    PhysicsCommandTicket AttachShape(PhysicsBodyReference body, CollisionGeometry3D geometry, const ShapeSettings& settings = {});
    PhysicsCommandTicket AttachShape(PhysicsBodyReference body, CollisionGeometry2D geometry, const ShapeSettings2D& settings = {});
    PhysicsCommandTicket ReplaceShape(PhysicsBodyReference body, PhysicsShapeReference previous, CollisionGeometry3D geometry, const ShapeSettings& settings = {});
    PhysicsCommandTicket ReplaceShape(PhysicsBodyReference body, PhysicsShapeReference previous, CollisionGeometry2D geometry, const ShapeSettings2D& settings = {});
    // Publishes all new shapes, then removes the old set. A failed attachment
    // rolls back every new shape and retains all previous handles.
    PhysicsCommandTicket ReplaceShapes(PhysicsBodyReference body, std::vector<PhysicsShapeReference> previous,
        std::vector<PhysicsShapeDescription3D> shapes);
    PhysicsCommandTicket ReplaceShapes2D(PhysicsBodyReference body, std::vector<PhysicsShapeReference> previous,
        std::vector<PhysicsShapeDescription2D> shapes);
    PhysicsCommandTicket UpdateDynamics(PhysicsBodyReference body, const BodySettings& settings);
    PhysicsCommandTicket UpdateDynamics(PhysicsBodyReference body, const BodySettings2D& settings);
    PhysicsCommandTicket RemoveBody(PhysicsBodyReference body);
    PhysicsCommandTicket RemoveShape(PhysicsShapeReference shape);
    PhysicsCommandTicket SetGravity(Maths::Vector3 gravity);
    PhysicsCommandTicket SetGravity(Maths::Vector2 gravity);
    PhysicsCommandTicket SetSolverSubsteps(uint32_t subdivisions);

    PhysicsCommandCommitReport Commit();
    PhysicsCommandResult GetResult(PhysicsCommandTicket ticket) const;
    size_t GetPendingCount() const;
    size_t GetResultCount() const;
    bool IsCommitting() const;
    // Cancel pending records and release result slots. Does not remove applied
    // native objects. Old tickets become invalid; a running commit refuses Reset.
    bool Reset();
private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
