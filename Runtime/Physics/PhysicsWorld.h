#pragma once

#include "CollisionMesh.h"

#include <cstdint>
#include <array>
#include <compare>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <Math/Quaternion.h>
#include <Math/Vector3.h>
#include "PhysicsDef.h"
#include "PhysicsAccess.h"
#include "PhysicsQuery.h"

namespace NLS::Base::TaskGraph { class TaskGraph; }

namespace NLS::Physics
{
struct BodyHandle
{
    uint64_t world = 0;
    uint32_t index = ~0u;
    uint32_t generation = 0;
    [[nodiscard]] bool IsValid() const { return world != 0 && index != ~0u && generation != 0; }
    auto operator<=>(const BodyHandle&) const = default;
};

struct ShapeHandle
{
    uint64_t world = 0;
    uint32_t index = ~0u, generation = 0;
    [[nodiscard]] bool IsValid() const { return world != 0 && index != ~0u && generation != 0; }
    auto operator<=>(const ShapeHandle&) const = default;
};

enum class ForceMode : uint8_t { Force, Acceleration, Impulse, VelocityChange };
struct WorldSettings
{
    Maths::Vector3 gravity{0, -9.81f, 0};
    uint32_t workerCount = 0;
    std::shared_ptr<Base::TaskGraph::TaskGraph> tasks;
};

struct BodySettings
{
    Maths::Vector3 position;
    Maths::Quaternion rotation = Maths::Quaternion::Identity;
    // Local shape pose applied on top of the body pose. Capsule geometry is
    // X-aligned in PhysX, so a Y-up collider rotates the shape by 90 degrees.
    Maths::Quaternion localRotation = Maths::Quaternion::Identity;
    uint32_t layer = 1;
    uint32_t material = 0;
    bool dynamic = false;
    bool kinematic = true;
    bool useGravity = true;
    float mass = 1.0f;
    Maths::Vector3 velocity;
    bool trigger = false;
    Maths::Vector3 angularVelocity;
    float linearDamping = 0, angularDamping = 0.05f;
    bool continuousCollision = false, startAwake = true;
    bool lockPositionX = false, lockPositionY = false, lockPositionZ = false;
    bool lockRotationX = false, lockRotationY = false, lockRotationZ = false;
    bool autoMassProperties = true;
    Maths::Vector3 centerOfMass;
    Maths::Quaternion massFrameRotation = Maths::Quaternion::Identity;
    Maths::Vector3 inertiaTensor{1,1,1};
    float sleepThreshold = 0.005f;
};

enum class MaterialCombineMode : uint8_t { Average, Minimum, Multiply, Maximum };
struct ShapeSettings
{
    Maths::Vector3 localPosition;
    Maths::Quaternion localRotation = Maths::Quaternion::Identity;
    uint32_t layer = 1, collisionMask = ~0u, material = 0;
    uint64_t owner = 0;
    float staticFriction = 0.5f, dynamicFriction = 0.5f, restitution = 0.5f;
    MaterialCombineMode frictionCombine = MaterialCombineMode::Average;
    MaterialCombineMode restitutionCombine = MaterialCombineMode::Average;
    bool trigger = false;
    uint32_t subShapeId = 0;
};

struct BodyState
{
    BodyHandle body;
    Maths::Vector3 position, velocity, angularVelocity, inertia;
    Maths::Quaternion rotation = Maths::Quaternion::Identity;
    float mass = 0;
    bool awake = false;
    uint64_t stepIndex = 0, poseRevision = 0;
    Maths::Vector3 centerOfMass;
    Maths::Quaternion massFrameRotation = Maths::Quaternion::Identity;
};

struct PhysicsState
{
    uint64_t world = 0, stepIndex = 0, topologyRevision = 0;
    std::vector<BodyState> bodies;
};

struct PhysicsStatistics
{
    uint64_t stepIndex = 0;
    uint32_t bodyCount = 0, shapeCount = 0, activeBodyCount = 0, sleepingBodyCount = 0;
    double stepMilliseconds = 0;
};

struct QueryFilter
{
    uint32_t layers = ~0u;
    bool includeStatic = true;
    bool includeDynamic = true;
    bool includeTriggers = false;
    std::vector<BodyHandle> ignored;
};

struct QueryHit
{
    BodyHandle body;
    Maths::Vector3 position;
    Maths::Vector3 normal;
    float distance = 0;
    uint32_t faceIndex = ~0u;
    uint32_t material = 0;
    ShapeHandle shape;
    uint64_t owner = 0;
    bool hasPointAndNormal = false;
    bool initialOverlap = false;
    uint32_t subShapeId = 0;
};

enum class QueryGeometryType3D : uint8_t { Sphere, Box, Capsule, Shape, Cylinder };
struct QueryGeometry3D
{
    QueryGeometryType3D type = QueryGeometryType3D::Sphere;
    Maths::Vector3 halfExtents{0.5f,0.5f,0.5f};
    float radius = 0.5f, halfHeight = 0.5f;
    ShapeHandle shape;
    bool preferPreciseSweep = true;
};
enum class ShapeQueryStatus : uint8_t { Success, InvalidInput, InvalidShape, UnsupportedGeometry };
struct ShapeQueryBufferResult
{
    QueryBufferResult buffer;
    ShapeQueryStatus status = ShapeQueryStatus::Success;
};

enum class ContactPhase : uint8_t { Enter, Stay, Exit };
struct ContactEvent
{
    BodyHandle first, second;
    ContactPhase phase = ContactPhase::Enter;
    bool trigger = false;
    ShapeHandle firstShape, secondShape;
    uint64_t firstOwner = 0, secondOwner = 0, stepIndex = 0;
    struct Point { Maths::Vector3 position, normal, impulse; float separation = 0; };
    std::array<Point, 4> points{};
    uint32_t pointCount = 0;
    bool hasContactData = false;
    uint32_t firstSubShapeId = 0, secondSubShapeId = 0;
};

class NLS_PHYSICS_API PhysicsWorld final
{
public:
    class ReadAccess;
    static std::shared_ptr<PhysicsWorld> Create(std::string& error);
    static std::shared_ptr<PhysicsWorld> Create(const WorldSettings& settings, std::string& error);
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    BodyHandle CreateBody(const BodySettings& settings = {});
    // Changes dynamics properties in place; preserves pose, velocities and forces.
    // Switching between static and dynamic native actor kinds requires rebuilding.
    bool UpdateDynamics(BodyHandle body, const BodySettings& settings);
    ShapeHandle AttachBox(BodyHandle body, Maths::Vector3 halfExtents, const ShapeSettings& settings = {});
    ShapeHandle AttachSphere(BodyHandle body, float radius, const ShapeSettings& settings = {});
    // Native capsule axis is X. Engine components supply the Y-axis alignment.
    ShapeHandle AttachCapsule(BodyHandle body, float radius, float halfHeight, const ShapeSettings& settings = {});
    ShapeHandle AttachCylinder(BodyHandle body, float radius, float halfHeight, const ShapeSettings& settings = {});
    ShapeHandle AttachConvexMesh(BodyHandle body, std::span<const Maths::Vector3> vertices, const ShapeSettings& settings = {});
    ShapeHandle AttachCollisionMesh(BodyHandle body, const std::shared_ptr<const CookedCollisionMesh>& mesh,
        const ShapeSettings& settings = {}, Maths::Vector3 scale = {1,1,1});
    ShapeHandle AttachTriangleMesh(BodyHandle body, std::span<const Maths::Vector3> vertices,
        std::span<const uint32_t> indices, const ShapeSettings& settings = {});
    // Row-major heights in meters. Rows advance along X; columns along Z.
    ShapeHandle AttachHeightField(BodyHandle body, uint32_t rows, uint32_t columns, std::span<const float> heights,
        float rowSpacing = 1, float columnSpacing = 1, const ShapeSettings& settings = {});
    BodyHandle AddBox(Maths::Vector3 halfExtents, const BodySettings& settings = {});
    BodyHandle AddSphere(float radius, const BodySettings& settings = {});
    BodyHandle AddCapsule(float radius, float halfHeight, const BodySettings& settings = {});
    BodyHandle AddTriangleMesh(std::span<const Maths::Vector3> vertices, std::span<const uint32_t> indices,
        const BodySettings& settings = {});
    bool Remove(BodyHandle handle);
    bool Remove(ShapeHandle handle);
    bool IsValid(BodyHandle handle) const;
    bool IsValid(ShapeHandle handle) const;
    BodyHandle GetShapeBody(ShapeHandle handle) const;
    bool SetTransform(BodyHandle handle, Maths::Vector3 position, Maths::Quaternion rotation);
    bool GetTransform(BodyHandle handle, Maths::Vector3& position, Maths::Quaternion& rotation) const;
    bool SetVelocity(BodyHandle handle, Maths::Vector3 velocity);
    Maths::Vector3 GetVelocity(BodyHandle handle) const;
    bool AddForce(BodyHandle handle, Maths::Vector3 force);
    bool ApplyForce(BodyHandle handle, Maths::Vector3 force, ForceMode mode);
    bool AddForceAtPosition(BodyHandle handle, Maths::Vector3 force, Maths::Vector3 position, ForceMode mode = ForceMode::Force);
    bool AddTorque(BodyHandle handle, Maths::Vector3 torque, ForceMode mode = ForceMode::Force);
    bool SetAngularVelocity(BodyHandle handle, Maths::Vector3 velocity);
    bool SetKinematicTarget(BodyHandle handle, Maths::Vector3 position, Maths::Quaternion rotation);
    bool SetAwake(BodyHandle handle, bool awake);
    bool GetState(BodyHandle handle, BodyState& state) const;
    bool SetGravity(Maths::Vector3 gravity);
    PhysicsStatistics GetStatistics() const;
    // O(1) change token; reads do not advance it. Not a simulation step count.
    uint64_t GetRevision() const noexcept;
    PhysicsState CaptureState() const;
    bool RestoreState(const PhysicsState& state, std::string& error);
    void Step(float fixedDeltaTime);
    std::vector<ContactEvent> TakeContactEvents();
    [[nodiscard]] std::unique_ptr<PhysicsMutationBatch> BeginMutationBatch();
    // Release read views before mutating the world on the same thread.
    [[nodiscard]] std::unique_ptr<ReadAccess> Read(const QueryFilter& filter = {}) const;

    class NLS_PHYSICS_API ReadAccess final
    {
    public:
        ~ReadAccess();
        ReadAccess(const ReadAccess&) = delete;
        ReadAccess& operator=(const ReadAccess&) = delete;
        bool Raycast(Maths::Vector3 origin, Maths::Vector3 direction, float distance, QueryHit& hit) const;
        std::vector<QueryHit> RaycastAll(Maths::Vector3 origin, Maths::Vector3 direction, float distance) const;
        QueryBufferResult RaycastNonAlloc(Maths::Vector3 origin, Maths::Vector3 direction, float distance, std::span<QueryHit> output) const;
        bool SweepSphere(float radius, Maths::Vector3 origin, Maths::Vector3 direction, float distance, QueryHit& hit) const;
        std::vector<BodyHandle> OverlapSphere(float radius, Maths::Vector3 center) const;
        QueryBufferResult OverlapSphereNonAlloc(float radius, Maths::Vector3 center, std::span<QueryHit> output) const;
        // Primitive capsules/cylinders are Y-aligned. Shape templates use their native
        // geometry axes, exclude their owning body and never cook during reads.
        bool GetShapeWorldPose(ShapeHandle shape, Maths::Vector3& position, Maths::Quaternion& rotation) const;
        bool SweepShape(const QueryGeometry3D& geometry, Maths::Vector3 origin, Maths::Quaternion rotation,
            Maths::Vector3 direction, float distance, QueryHit& hit, ShapeQueryStatus* status = nullptr) const;
        std::vector<QueryHit> SweepShapeAll(const QueryGeometry3D& geometry, Maths::Vector3 origin, Maths::Quaternion rotation,
            Maths::Vector3 direction, float distance, ShapeQueryStatus* status = nullptr) const;
        ShapeQueryBufferResult SweepShapeNonAlloc(const QueryGeometry3D& geometry, Maths::Vector3 origin, Maths::Quaternion rotation,
            Maths::Vector3 direction, float distance, std::span<QueryHit> output) const;
        std::vector<QueryHit> OverlapShape(const QueryGeometry3D& geometry, Maths::Vector3 position, Maths::Quaternion rotation,
            ShapeQueryStatus* status = nullptr) const;
        ShapeQueryBufferResult OverlapShapeNonAlloc(const QueryGeometry3D& geometry, Maths::Vector3 position, Maths::Quaternion rotation,
            std::span<QueryHit> output) const;

    private:
        struct Impl;
        explicit ReadAccess(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> m_impl;
        friend class PhysicsWorld;
    };

private:
    struct State;
    explicit PhysicsWorld(std::shared_ptr<State> state);
    std::shared_ptr<State> m_state;
};
}
