#pragma once

#include "PhysicsDef.h"
#include "PhysicsAccess.h"
#include "PhysicsQuery.h"
#include <Math/Vector2.h>
#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace NLS::Base::TaskGraph { class TaskGraph; }

namespace NLS::Physics
{
struct BodyHandle2D
{
    uint64_t world = 0;
    uint32_t index = ~0u, generation = 0;
    bool IsValid() const { return world != 0 && index != ~0u && generation != 0; }
    auto operator<=>(const BodyHandle2D&) const = default;
};
struct ShapeHandle2D
{
    uint64_t world = 0;
    uint32_t index = ~0u, generation = 0;
    bool IsValid() const { return world != 0 && index != ~0u && generation != 0; }
    auto operator<=>(const ShapeHandle2D&) const = default;
};
enum class BodyType2D : uint8_t { Static, Dynamic, Kinematic };
enum class ForceMode2D : uint8_t { Force, Acceleration, Impulse, VelocityChange };
struct WorldSettings2D
{
    Maths::Vector2 gravity{0, -9.81f};
    uint32_t solverSubsteps = 4;
    uint32_t workerCount = 0;
    std::shared_ptr<Base::TaskGraph::TaskGraph> tasks;
};
struct BodySettings2D
{
    BodyType2D type = BodyType2D::Static;
    Maths::Vector2 position, velocity;
    float rotation = 0, angularVelocity = 0;
    float linearDamping = 0, angularDamping = 0, gravityScale = 1;
    float mass = 1;
    bool autoMass = true, allowSleep = true, startAwake = true, bullet = false;
    bool lockPositionX = false, lockPositionY = false, lockRotation = false;
};
struct ShapeSettings2D
{
    Maths::Vector2 offset;
    float rotation = 0;
    float density = 1, friction = 0.5f, restitution = 0;
    uint32_t layers = 1, collisionMask = ~0u;
    uint64_t owner = 0;
    bool trigger = false;
    uint32_t subShapeId = 0;
};
struct BodyState2D
{
    BodyHandle2D body;
    Maths::Vector2 position, velocity;
    float rotation = 0, angularVelocity = 0, mass = 0, inertia = 0;
    bool awake = false;
    uint64_t stepIndex = 0, poseRevision = 0;
};
struct PhysicsState2D
{
    uint64_t world = 0, stepIndex = 0, topologyRevision = 0;
    std::vector<BodyState2D> bodies;
};
struct QueryFilter2D
{
    uint32_t layers = ~0u;
    bool includeTriggers = false;
    bool includeStatic = true, includeDynamic = true;
    std::vector<BodyHandle2D> ignored;
};
struct QueryHit2D
{
    BodyHandle2D body;
    ShapeHandle2D shape;
    uint64_t owner = 0;
    Maths::Vector2 position, normal;
    float distance = 0, fraction = 0;
    bool hasPointAndNormal = false;
    uint32_t subShapeId = 0;
};
struct PhysicsStatistics2D
{
    uint64_t stepIndex = 0;
    uint32_t bodyCount = 0, shapeCount = 0, activeBodyCount = 0, sleepingBodyCount = 0;
    uint32_t contactCount = 0, jointCount = 0;
    double stepMilliseconds = 0;
};

// Public coordinates are meters; rotations and angular velocities use radians.
// Backend IDs and native pointers never cross this boundary. Read views retain
// the world; release a view before mutating that world on the same thread.
class NLS_PHYSICS_API PhysicsWorld2D final
{
public:
    class ReadAccess;
    static std::shared_ptr<PhysicsWorld2D> Create(std::string& error);
    static std::shared_ptr<PhysicsWorld2D> Create(const WorldSettings2D& settings, std::string& error);
    ~PhysicsWorld2D();
    PhysicsWorld2D(const PhysicsWorld2D&) = delete;
    PhysicsWorld2D& operator=(const PhysicsWorld2D&) = delete;

    BodyHandle2D CreateBody(const BodySettings2D& settings = {});
    bool UpdateDynamics(BodyHandle2D body, const BodySettings2D& settings);
    bool Remove(BodyHandle2D body);
    bool Remove(ShapeHandle2D shape);
    bool IsValid(BodyHandle2D body) const;
    bool IsValid(ShapeHandle2D shape) const;
    BodyHandle2D GetShapeBody(ShapeHandle2D shape) const;
    ShapeHandle2D AddBox(BodyHandle2D body, Maths::Vector2 halfExtents, const ShapeSettings2D& settings = {});
    ShapeHandle2D AddCircle(BodyHandle2D body, float radius, const ShapeSettings2D& settings = {});
    ShapeHandle2D AddCapsule(BodyHandle2D body, float radius, float halfHeight, const ShapeSettings2D& settings = {});
    ShapeHandle2D AddConvexPolygon(BodyHandle2D body, std::span<const Maths::Vector2> vertices, const ShapeSettings2D& settings = {});
    ShapeHandle2D AddSegment(BodyHandle2D body, Maths::Vector2 first, Maths::Vector2 second, const ShapeSettings2D& settings = {});
    // One-sided, static-only segment; points are previous, first, second, next.
    // Ghost vertices suppress internal-edge contacts. Sensors are not supported.
    ShapeHandle2D AddChainSegment(BodyHandle2D body, std::span<const Maths::Vector2> points, const ShapeSettings2D& settings = {});
    bool GetState(BodyHandle2D body, BodyState2D& state) const;
    bool SetTransform(BodyHandle2D body, Maths::Vector2 position, float rotation, bool keepVelocity = true);
    bool SetVelocity(BodyHandle2D body, Maths::Vector2 linear, float angular = 0);
    bool AddForce(BodyHandle2D body, Maths::Vector2 force, ForceMode2D mode = ForceMode2D::Force);
    bool AddForceAtPosition(BodyHandle2D body, Maths::Vector2 force, Maths::Vector2 position, ForceMode2D mode = ForceMode2D::Force);
    bool AddTorque(BodyHandle2D body, float torque, ForceMode2D mode = ForceMode2D::Force);
    bool SetAwake(BodyHandle2D body, bool awake);
    bool SetGravity(Maths::Vector2 gravity);
    bool SetSolverSubsteps(uint32_t subdivisions);
    uint32_t GetSolverSubsteps() const;
    void Step(float fixedDeltaTime);
    PhysicsStatistics2D GetStatistics() const;
    uint64_t GetRevision() const noexcept;
    PhysicsState2D CaptureState() const;
    bool RestoreState(const PhysicsState2D& state, std::string& error);
    [[nodiscard]] std::unique_ptr<PhysicsMutationBatch> BeginMutationBatch();
    std::unique_ptr<ReadAccess> Read(const QueryFilter2D& filter = {}) const;

    class NLS_PHYSICS_API ReadAccess final
    {
    public:
        ~ReadAccess();
        bool Raycast(Maths::Vector2 origin, Maths::Vector2 direction, float distance, QueryHit2D& hit) const;
        std::vector<QueryHit2D> RaycastAll(Maths::Vector2 origin, Maths::Vector2 direction, float distance) const;
        QueryBufferResult RaycastNonAlloc(Maths::Vector2 origin, Maths::Vector2 direction, float distance, std::span<QueryHit2D> output) const;
        std::vector<QueryHit2D> OverlapCircle(Maths::Vector2 center, float radius) const;
        QueryBufferResult OverlapCircleNonAlloc(Maths::Vector2 center, float radius, std::span<QueryHit2D> output) const;
        bool CircleCast(Maths::Vector2 origin, float radius, Maths::Vector2 direction, float distance, QueryHit2D& hit) const;
    private:
        struct Impl;
        explicit ReadAccess(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> m_impl;
        friend class PhysicsWorld2D;
    };
private:
    struct State;
    explicit PhysicsWorld2D(std::shared_ptr<State> state);
    std::shared_ptr<State> m_state;
};
}
