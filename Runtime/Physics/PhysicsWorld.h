#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <Math/Quaternion.h>
#include <Math/Vector3.h>
#include "PhysicsDef.h"

namespace NLS::Physics
{
struct BodyHandle
{
    uint64_t world = 0;
    uint32_t index = ~0u;
    uint32_t generation = 0;
    [[nodiscard]] bool IsValid() const { return world != 0 && index != ~0u && generation != 0; }
    bool operator==(const BodyHandle&) const = default;
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
    bool trigger = false;
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
};

class NLS_PHYSICS_API PhysicsWorld final
{
public:
    class ReadAccess;
    static std::shared_ptr<PhysicsWorld> Create(std::string& error);
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    BodyHandle AddBox(Maths::Vector3 halfExtents, const BodySettings& settings = {});
    BodyHandle AddSphere(float radius, const BodySettings& settings = {});
    BodyHandle AddCapsule(float radius, float halfHeight, const BodySettings& settings = {});
    BodyHandle AddTriangleMesh(std::span<const Maths::Vector3> vertices, std::span<const uint32_t> indices,
        const BodySettings& settings = {});
    bool Remove(BodyHandle handle);
    bool SetTransform(BodyHandle handle, Maths::Vector3 position, Maths::Quaternion rotation);
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
        bool SweepSphere(float radius, Maths::Vector3 origin, Maths::Vector3 direction, float distance, QueryHit& hit) const;
        std::vector<BodyHandle> OverlapSphere(float radius, Maths::Vector3 center) const;

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
