#include "PhysicsWorld.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

#include <PxPhysicsAPI.h>

namespace NLS::Physics
{
namespace
{
using namespace physx;
PxVec3 ToNative(Maths::Vector3 vector) { return {vector.x,vector.y,vector.z}; }
Maths::Vector3 FromNative(PxVec3 vector) { return {vector.x,vector.y,vector.z}; }
bool Finite(Maths::Vector3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
bool ValidRotation(Maths::Quaternion value)
{
    const auto length = value.x*value.x+value.y*value.y+value.z*value.z+value.w*value.w;
    return std::isfinite(length) && length > 1e-12f;
}
PxTransform Pose(Maths::Vector3 position, Maths::Quaternion rotation)
{
    auto quaternion = PxQuat(rotation.x,rotation.y,rotation.z,rotation.w);
    quaternion.normalize();
    return {ToNative(position),quaternion};
}
bool ValidRay(Maths::Vector3 origin, Maths::Vector3 direction, float distance)
{
    return Finite(origin) && Finite(direction) && std::isfinite(direction.LengthSquared()) && direction.LengthSquared() > 1e-12f &&
        std::isfinite(distance) && distance > 0;
}

class ImmediateDispatcher final : public PxCpuDispatcher
{
public:
    void submitTask(PxBaseTask& task) override { task.run(); task.release(); }
    PxU32 getWorkerCount() const override { return 0; }
};

struct Sdk
{
    PxDefaultAllocator allocator;
    PxDefaultErrorCallback errors;
    PxFoundation* foundation = nullptr;
    PxPhysics* physics = nullptr;
    ImmediateDispatcher dispatcher;
    Sdk()
    {
        foundation = PxCreateFoundation(PX_PHYSICS_VERSION,allocator,errors);
        if (!foundation) throw std::runtime_error("PhysX foundation initialization failed");
        physics = PxCreatePhysics(PX_PHYSICS_VERSION,*foundation,PxTolerancesScale(),false,nullptr);
        if (!physics) { foundation->release(); foundation = nullptr; throw std::runtime_error("PhysX SDK initialization failed"); }
    }
    ~Sdk()
    {
        if (physics) physics->release();
        if (foundation) foundation->release();
    }
};

std::shared_ptr<Sdk> AcquireSdk()
{
    static const auto sdk = std::make_shared<Sdk>();
    return sdk;
}

struct BodyRecord
{
    BodyHandle handle;
    PxRigidActor* actor = nullptr;
    BodySettings settings;
    std::vector<uint32_t> faceRemap;
};

class Filter final : public PxQueryFilterCallback
{
public:
    explicit Filter(const QueryFilter& filter, bool all = false) : m_filter(filter),m_all(all) {}
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape*, const PxRigidActor* actor, PxHitFlags&) override
    {
        const auto* body = static_cast<const BodyRecord*>(actor->userData);
        if (!body || !(body->settings.layer & m_filter.layers) || (body->settings.trigger && !m_filter.includeTriggers) ||
            std::find(m_filter.ignored.begin(),m_filter.ignored.end(),body->handle) != m_filter.ignored.end()) return PxQueryHitType::eNONE;
        return m_all ? PxQueryHitType::eTOUCH : PxQueryHitType::eBLOCK;
    }
    PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override
    { return m_all ? PxQueryHitType::eTOUCH : PxQueryHitType::eBLOCK; }
private:
    const QueryFilter& m_filter;
    bool m_all;
};

PxQueryFilterData QueryFlags(const QueryFilter& filter)
{
    PxQueryFlags flags = PxQueryFlag::ePREFILTER;
    if (filter.includeStatic) flags |= PxQueryFlag::eSTATIC;
    if (filter.includeDynamic) flags |= PxQueryFlag::eDYNAMIC;
    return PxQueryFilterData(flags);
}

template<class Hit>
QueryHit Convert(const Hit& native)
{
    QueryHit hit;
    const auto* body = static_cast<const BodyRecord*>(native.actor->userData);
    if (body) { hit.body = body->handle; hit.material = body->settings.material; }
    hit.position = FromNative(native.position); hit.normal = FromNative(native.normal);
    hit.distance = native.distance; hit.faceIndex = native.faceIndex;
    if (body && hit.faceIndex < body->faceRemap.size()) hit.faceIndex = body->faceRemap[hit.faceIndex];
    return hit;
}
}

struct PhysicsWorld::State
{
    std::shared_ptr<Sdk> sdk;
    PxScene* scene = nullptr;
    PxMaterial* material = nullptr;
    uint64_t world = 0;
    mutable std::shared_mutex mutex;
    std::vector<std::unique_ptr<BodyRecord>> bodies;
    std::vector<uint32_t> generations;
    ~State()
    {
        for (const auto& body : bodies) if (body && body->actor) body->actor->release();
        if (scene) scene->release();
        if (material) material->release();
    }

    BodyHandle Add(const PxGeometry& geometry, const BodySettings& settings, std::span<const uint32_t> faceRemap = {})
    {
        if (!Finite(settings.position) || !ValidRotation(settings.rotation) || !ValidRotation(settings.localRotation)) return {};
        std::unique_lock lock(mutex);
        auto* actor = settings.dynamic ? static_cast<PxRigidActor*>(sdk->physics->createRigidDynamic(Pose(settings.position,settings.rotation)))
                                      : static_cast<PxRigidActor*>(sdk->physics->createRigidStatic(Pose(settings.position,settings.rotation)));
        if (!actor) return {};
        if (settings.dynamic) static_cast<PxRigidDynamic*>(actor)->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC,true);
        auto* shape = sdk->physics->createShape(geometry,*material,true,PxShapeFlag::eSCENE_QUERY_SHAPE);
        if (!shape) { actor->release(); return {}; }
        shape->setLocalPose(Pose({0,0,0},settings.localRotation));
        if (!actor->attachShape(*shape))
        { shape->release(); actor->release(); return {}; }
        shape->release();
        uint32_t index = 0;
        while (index < bodies.size() && bodies[index]) ++index;
        if (index == bodies.size()) { bodies.emplace_back(); generations.push_back(1); }
        auto body = std::make_unique<BodyRecord>();
        body->handle = {world,index,generations[index]};
        body->settings = settings;
        body->faceRemap.assign(faceRemap.begin(),faceRemap.end());
        body->actor = actor;
        actor->userData = body.get();
        const auto handle = body->handle;
        bodies[index] = std::move(body);
        scene->addActor(*actor);
        scene->flushQueryUpdates();
        return handle;
    }

    BodyRecord* Find(BodyHandle handle) const
    {
        if (handle.world != world || handle.index >= bodies.size() || generations[handle.index] != handle.generation) return nullptr;
        return bodies[handle.index].get();
    }
};

struct PhysicsWorld::ReadAccess::Impl
{
    std::shared_ptr<State> state;
    std::shared_lock<std::shared_mutex> lock;
    QueryFilter filter;
    Impl(std::shared_ptr<State> value, QueryFilter queryFilter) : state(std::move(value)),lock(state->mutex),filter(std::move(queryFilter)) {}
};

PhysicsWorld::PhysicsWorld(std::shared_ptr<State> state) : m_state(std::move(state)) {}
PhysicsWorld::~PhysicsWorld() = default;

std::shared_ptr<PhysicsWorld> PhysicsWorld::Create(std::string& error)
{
    try
    {
        auto state = std::make_shared<State>();
        state->sdk = AcquireSdk();
        PxSceneDesc desc(state->sdk->physics->getTolerancesScale());
        desc.gravity = {0,-9.81f,0};
        desc.cpuDispatcher = &state->sdk->dispatcher;
        desc.filterShader = PxDefaultSimulationFilterShader;
        state->scene = state->sdk->physics->createScene(desc);
        state->material = state->sdk->physics->createMaterial(0.5f,0.5f,0.5f);
        if (!state->scene || !state->material) throw std::runtime_error("PhysX query scene creation failed");
        static std::atomic<uint64_t> next{1};
        state->world = next.fetch_add(1);
        error.clear();
        return std::shared_ptr<PhysicsWorld>(new PhysicsWorld(std::move(state)));
    }
    catch (const std::exception& exception) { error = exception.what(); return {}; }
}

BodyHandle PhysicsWorld::AddBox(Maths::Vector3 halfExtents, const BodySettings& settings)
{
    if (!Finite(halfExtents) || halfExtents.x <= 0 || halfExtents.y <= 0 || halfExtents.z <= 0) return {};
    return m_state->Add(PxBoxGeometry(ToNative(halfExtents)),settings);
}
BodyHandle PhysicsWorld::AddSphere(float radius, const BodySettings& settings)
{
    if (!std::isfinite(radius) || radius <= 0) return {};
    return m_state->Add(PxSphereGeometry(radius),settings);
}
BodyHandle PhysicsWorld::AddCapsule(float radius, float halfHeight, const BodySettings& settings)
{
    if (!std::isfinite(radius) || radius <= 0 || !std::isfinite(halfHeight) || halfHeight < 0) return {};
    return m_state->Add(PxCapsuleGeometry(radius,halfHeight),settings);
}
BodyHandle PhysicsWorld::AddTriangleMesh(std::span<const Maths::Vector3> vertices, std::span<const uint32_t> indices,
    const BodySettings& settings)
{
    if (vertices.size() < 3 || indices.empty() || indices.size()%3 || vertices.size() > UINT32_MAX || indices.size()/3 > UINT32_MAX ||
        settings.dynamic || !std::all_of(vertices.begin(),vertices.end(),Finite)) return {};
    for (const auto index : indices) if (index >= vertices.size()) return {};
    std::vector<PxVec3> points;
    points.reserve(vertices.size());
    for (const auto vertex : vertices) points.push_back(ToNative(vertex));
    PxTriangleMeshDesc desc;
    desc.points.count = static_cast<PxU32>(points.size()); desc.points.stride = sizeof(PxVec3); desc.points.data = points.data();
    desc.triangles.count = static_cast<PxU32>(indices.size()/3); desc.triangles.stride = sizeof(uint32_t)*3; desc.triangles.data = indices.data();
    PxCookingParams params(m_state->sdk->physics->getTolerancesScale());
    params.suppressTriangleMeshRemapTable = false;
    auto* mesh = PxCreateTriangleMesh(params,desc,m_state->sdk->physics->getPhysicsInsertionCallback());
    if (!mesh) return {};
    const auto* remap = mesh->getTrianglesRemap();
    const auto handle = m_state->Add(PxTriangleMeshGeometry(mesh),settings,
        remap ? std::span<const uint32_t>(remap,mesh->getNbTriangles()) : std::span<const uint32_t>{});
    mesh->release();
    return handle;
}

bool PhysicsWorld::Remove(BodyHandle handle)
{
    std::unique_lock lock(m_state->mutex);
    auto* body = m_state->Find(handle);
    if (!body) return false;
    m_state->scene->removeActor(*body->actor);
    body->actor->release(); body->actor = nullptr;
    m_state->bodies[handle.index].reset();
    if (++m_state->generations[handle.index] == 0) ++m_state->generations[handle.index];
    m_state->scene->flushQueryUpdates();
    return true;
}
bool PhysicsWorld::SetTransform(BodyHandle handle, Maths::Vector3 position, Maths::Quaternion rotation)
{
    if (!Finite(position) || !ValidRotation(rotation)) return false;
    std::unique_lock lock(m_state->mutex);
    auto* body = m_state->Find(handle);
    if (!body) return false;
    body->actor->setGlobalPose(Pose(position,rotation));
    m_state->scene->flushQueryUpdates();
    return true;
}
std::unique_ptr<PhysicsWorld::ReadAccess> PhysicsWorld::Read(const QueryFilter& filter) const
{
    return std::unique_ptr<ReadAccess>(new ReadAccess(std::make_unique<ReadAccess::Impl>(m_state,filter)));
}
PhysicsWorld::ReadAccess::ReadAccess(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
PhysicsWorld::ReadAccess::~ReadAccess() = default;

bool PhysicsWorld::ReadAccess::Raycast(Maths::Vector3 origin, Maths::Vector3 direction, float distance, QueryHit& hit) const
{
    if (!ValidRay(origin,direction,distance) || (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic)) return false;
    Filter filter(m_impl->filter);
    PxRaycastBuffer buffer;
    if (!m_impl->state->scene->raycast(ToNative(origin),ToNative(direction.Normalised()),distance,buffer,
        PxHitFlag::eDEFAULT,QueryFlags(m_impl->filter),&filter) || !buffer.hasBlock) return false;
    hit = Convert(buffer.block); return true;
}

std::vector<QueryHit> PhysicsWorld::ReadAccess::RaycastAll(Maths::Vector3 origin, Maths::Vector3 direction, float distance) const
{
    std::vector<QueryHit> hits;
    if (!ValidRay(origin,direction,distance) || (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic)) return hits;
    struct Collector : PxRaycastCallback
    {
        PxRaycastHit storage[64];
        std::vector<QueryHit>& results;
        explicit Collector(std::vector<QueryHit>& output) : PxRaycastCallback(storage,64),results(output) {}
        PxAgain processTouches(const PxRaycastHit* values, PxU32 count) override
        { for (PxU32 index = 0; index < count; ++index) results.push_back(Convert(values[index])); return true; }
    } collector(hits);
    Filter filter(m_impl->filter,true);
    m_impl->state->scene->raycast(ToNative(origin),ToNative(direction.Normalised()),distance,collector,
        PxHitFlag::eDEFAULT,QueryFlags(m_impl->filter),&filter);
    for (PxU32 index = 0; index < collector.nbTouches; ++index) hits.push_back(Convert(collector.touches[index]));
    std::sort(hits.begin(),hits.end(),[](const QueryHit& left, const QueryHit& right) { return left.distance < right.distance; });
    return hits;
}

bool PhysicsWorld::ReadAccess::SweepSphere(float radius, Maths::Vector3 origin, Maths::Vector3 direction, float distance, QueryHit& hit) const
{
    if (!std::isfinite(radius) || radius <= 0 || !ValidRay(origin,direction,distance) ||
        (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic)) return false;
    Filter filter(m_impl->filter);
    PxSweepBuffer buffer;
    if (!m_impl->state->scene->sweep(PxSphereGeometry(radius),PxTransform(ToNative(origin)),ToNative(direction.Normalised()),distance,buffer,
        PxHitFlag::eDEFAULT,QueryFlags(m_impl->filter),&filter) || !buffer.hasBlock) return false;
    hit = Convert(buffer.block); return true;
}

std::vector<BodyHandle> PhysicsWorld::ReadAccess::OverlapSphere(float radius, Maths::Vector3 center) const
{
    std::vector<BodyHandle> hits;
    if (!std::isfinite(radius) || radius <= 0 || !Finite(center) || (!m_impl->filter.includeStatic && !m_impl->filter.includeDynamic)) return hits;
    struct Collector : PxOverlapCallback
    {
        PxOverlapHit storage[64];
        std::vector<BodyHandle>& results;
        explicit Collector(std::vector<BodyHandle>& output) : PxOverlapCallback(storage,64),results(output) {}
        PxAgain processTouches(const PxOverlapHit* values, PxU32 count) override
        { for (PxU32 i = 0; i < count; ++i) results.push_back(static_cast<BodyRecord*>(values[i].actor->userData)->handle); return true; }
    } collector(hits);
    Filter filter(m_impl->filter,true);
    m_impl->state->scene->overlap(PxSphereGeometry(radius),PxTransform(ToNative(center)),collector,QueryFlags(m_impl->filter),&filter);
    for (PxU32 index = 0; index < collector.nbTouches; ++index)
        hits.push_back(static_cast<BodyRecord*>(collector.touches[index].actor->userData)->handle);
    return hits;
}
}
