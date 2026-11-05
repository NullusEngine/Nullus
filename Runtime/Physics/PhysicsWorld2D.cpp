#include "PhysicsWorld2D.h"
#include "Geometry2D.h"

#include <box2d/box2d.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <TaskGraph/TaskGraph.h>

namespace NLS::Physics
{
namespace
{
std::mutex g_boxWorldMutex;
std::atomic<uint64_t> g_nextBoxWorld{1};
// Simulation categories occupy the public low 32 bits. A private query bit
// keeps the query layer mask independent from a shape's collision mask.
constexpr uint64_t kQueryCategory = uint64_t{1} << 63;
b2Vec2 Native(Maths::Vector2 v) { return {v.x, v.y}; }
Maths::Vector2 Engine(b2Vec2 v) { return {v.x, v.y}; }
bool Finite(Maths::Vector2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool Nonnegative(float value) { return std::isfinite(value) && value >= 0; }
b2BodyType Native(BodyType2D type)
{
    switch (type)
    {
    case BodyType2D::Dynamic: return b2_dynamicBody;
    case BodyType2D::Kinematic: return b2_kinematicBody;
    default: return b2_staticBody;
    }
}
struct BodyRecord2D
{
    BodyHandle2D handle;
    b2BodyId native{};
    BodySettings2D settings;
    std::vector<ShapeHandle2D> shapes;
    uint64_t poseRevision = 1;
};
struct ShapeRecord2D
{
    ShapeHandle2D handle;
    BodyHandle2D body;
    b2ShapeId native{};
    ShapeSettings2D settings;
};
BodyState2D ReadBodyState2D(const BodyRecord2D& body, uint64_t stepIndex)
{
    BodyState2D out; out.body=body.handle;
    out.stepIndex=stepIndex; out.poseRevision=body.poseRevision;
    out.position=Engine(b2Body_GetPosition(body.native)); out.rotation=b2Rot_GetAngle(b2Body_GetRotation(body.native));
    out.velocity=Engine(b2Body_GetLinearVelocity(body.native)); out.angularVelocity=b2Body_GetAngularVelocity(body.native);
    out.mass=b2Body_GetMass(body.native); out.inertia=b2Body_GetRotationalInertia(body.native); out.awake=b2Body_IsAwake(body.native);
    return out;
}
bool ValidShape(const ShapeSettings2D& s)
{
    return Detail::ValidGeometryPoint2D(s.offset) && std::isfinite(s.rotation) && Nonnegative(s.density) &&
        s.density<=1e4f && (s.density==0 || s.density>=1e-6f) &&
        Nonnegative(s.friction) && s.friction<=1e4f && Nonnegative(s.restitution) && s.restitution <= 1;
}
Maths::Vector2 LocalPoint(Maths::Vector2 value, const ShapeSettings2D& s)
{
    const float c = std::cos(s.rotation), sn = std::sin(s.rotation);
    return {s.offset.x + c * value.x - sn * value.y, s.offset.y + sn * value.x + c * value.y};
}
bool CastVector(Maths::Vector2 origin, Maths::Vector2 direction, float distance, b2Vec2& translation)
{
    if (!Finite(origin) || !Finite(direction) || !std::isfinite(distance) || distance <= 0) return false;
    const double length = std::hypot(static_cast<double>(direction.x), static_cast<double>(direction.y));
    if (length <= 1e-12) return false;
    translation = {static_cast<float>(direction.x / length * distance), static_cast<float>(direction.y / length * distance)};
    return true;
}
}

struct PhysicsWorld2D::State
{
    uint64_t world = 0;
    b2WorldId native{};
    WorldSettings2D settings;
    mutable Detail::WorldAccessMutex mutex;
    std::vector<std::unique_ptr<BodyRecord2D>> bodies;
    std::vector<std::unique_ptr<ShapeRecord2D>> shapes;
    std::vector<uint32_t> bodyGenerations, shapeGenerations;
    std::vector<uint32_t> freeBodies, freeShapes;
    PhysicsStatistics2D statistics;
    uint64_t topologyRevision = 0;
    struct TaskToken { Base::TaskGraph::TaskCompletion completion; };
    std::vector<std::unique_ptr<TaskToken>> taskTokens;
    size_t usedTasks = 0;
    std::mutex taskMutex;

    static void* Enqueue(b2TaskCallbackFcn* function, void* context, void* user)
    {
        auto& self = *static_cast<State*>(user);
        auto& graph = *self.settings.tasks;
        TaskToken* token;
        {
            std::lock_guard lock(self.taskMutex);
            if (self.usedTasks == self.taskTokens.size()) self.taskTokens.push_back(std::make_unique<TaskToken>());
            token = self.taskTokens[self.usedTasks++].get();
        }
        Base::TaskGraph::TaskDesc desc;
        desc.function = [function, context] { function(context); };
        desc.debugName = "Physics2D"; desc.cancellable = false;
        const auto submission = graph.SubmitTracked(std::move(desc));
        if (!submission.handle.IsValid()) { function(context); return nullptr; }
        token->completion = submission.completion;
        return token;
    }
    static void Finish(void* token, void*)
    {
        (void)static_cast<TaskToken*>(token)->completion.Wait();
    }

    ~State()
    {
        std::lock_guard lock(g_boxWorldMutex);
        if (B2_IS_NON_NULL(native)) b2DestroyWorld(native);
    }
    BodyRecord2D* Find(BodyHandle2D h) const
    {
        return h.world == world && h.index < bodies.size() && h.generation == bodyGenerations[h.index] ? bodies[h.index].get() : nullptr;
    }
    ShapeRecord2D* Find(ShapeHandle2D h) const
    {
        return h.world == world && h.index < shapes.size() && h.generation == shapeGenerations[h.index] ? shapes[h.index].get() : nullptr;
    }
    void UpdateMass(BodyRecord2D& body)
    {
        if (body.settings.type != BodyType2D::Dynamic || body.settings.autoMass) return;
        auto data = b2Body_GetMassData(body.native);
        const float ratio = data.mass > 0 ? body.settings.mass / data.mass : 0;
        data.rotationalInertia *= ratio;
        data.mass = body.settings.mass;
        b2Body_SetMassData(body.native, data);
    }
    template<typename Create>
    ShapeHandle2D AddShape(BodyHandle2D bodyHandle, const ShapeSettings2D& s, Create&& create)
    {
        if (!ValidShape(s)) return {};
        std::unique_lock lock(mutex);
        auto* body = Find(bodyHandle);
        if (!body) return {};
        auto shape = std::make_unique<ShapeRecord2D>();
        uint32_t index;
        if (freeShapes.empty())
        {
            index = static_cast<uint32_t>(shapes.size());
            shapes.emplace_back(); shapeGenerations.push_back(1);
        }
        else { index = freeShapes.back(); freeShapes.pop_back(); }
        shape->handle = {world, index, shapeGenerations[index]};
        shape->body = bodyHandle; shape->settings = s;
        auto def = b2DefaultShapeDef();
        def.density = s.density;
        def.material.friction = s.friction; def.material.restitution = s.restitution;
        def.filter.categoryBits = s.layers; def.filter.maskBits = s.collisionMask | kQueryCategory;
        def.isSensor = s.trigger; def.enableSensorEvents = true;
        def.enableContactEvents = true; def.enableHitEvents = true;
        def.userData = shape.get();
        body->shapes.reserve(body->shapes.size() + 1);
        shape->native = create(body->native, def);
        if (B2_IS_NULL(shape->native)) { freeShapes.push_back(index); return {}; }
        const auto handle = shape->handle;
        shapes[index] = std::move(shape); body->shapes.push_back(handle);
        if (++body->poseRevision == 0) ++body->poseRevision;
        ++topologyRevision;
        UpdateMass(*body);
        return handle;
    }
    bool RemoveShape(ShapeHandle2D handle)
    {
        auto* shape = Find(handle);
        if (!shape) return false;
        auto* body = Find(shape->body);
        b2DestroyShape(shape->native, true);
        if (body)
        {
            std::erase(body->shapes, handle); UpdateMass(*body);
            if (++body->poseRevision == 0) ++body->poseRevision;
        }
        shapes[handle.index].reset();
        if (++shapeGenerations[handle.index] == 0) ++shapeGenerations[handle.index];
        freeShapes.push_back(handle.index);
        ++topologyRevision;
        return true;
    }
};

PhysicsWorld2D::PhysicsWorld2D(std::shared_ptr<State> state) : m_state(std::move(state)) {}
PhysicsWorld2D::~PhysicsWorld2D() = default;
std::unique_ptr<PhysicsMutationBatch> PhysicsWorld2D::BeginMutationBatch()
{ return std::unique_ptr<PhysicsMutationBatch>(new PhysicsMutationBatch(m_state, m_state->mutex)); }
std::shared_ptr<PhysicsWorld2D> PhysicsWorld2D::Create(std::string& error) { return Create({}, error); }
std::shared_ptr<PhysicsWorld2D> PhysicsWorld2D::Create(const WorldSettings2D& settings, std::string& error)
{
    if (!Finite(settings.gravity) || settings.solverSubsteps == 0 || settings.solverSubsteps > 64)
    { error = "Invalid 2D world gravity or solver substep count (expected 1..64)."; return {}; }
    try
    {
        auto state = std::make_shared<State>();
        state->world = g_nextBoxWorld.fetch_add(1); state->settings = settings;
        auto def = b2DefaultWorldDef(); def.gravity = Native(settings.gravity); def.workerCount = 1;
        if (!state->settings.tasks && settings.workerCount > 0)
        {
            Base::TaskGraph::TaskGraphConfig taskSettings; taskSettings.workerCount = (std::min)(settings.workerCount, 31u);
            state->settings.tasks = std::make_shared<Base::TaskGraph::TaskGraph>(taskSettings);
            if (!state->settings.tasks->Start()) { error = "Cannot start the physics task graph."; return {}; }
        }
        if (state->settings.tasks)
        {
            if (!state->settings.tasks->IsStarted()) { error = "The supplied physics task graph is not started."; return {}; }
            def.workerCount = static_cast<int>((std::min)(state->settings.tasks->GetWorkerCount() + 1, 32u));
            def.enqueueTask = State::Enqueue; def.finishTask = State::Finish; def.userTaskContext = state.get();
        }
        { std::lock_guard lock(g_boxWorldMutex); state->native = b2CreateWorld(&def); }
        if (B2_IS_NULL(state->native)) { error = "Box2D world creation failed."; return {}; }
        error.clear();
        return std::shared_ptr<PhysicsWorld2D>(new PhysicsWorld2D(std::move(state)));
    }
    catch (const std::exception& e) { error = e.what(); return {}; }
}
BodyHandle2D PhysicsWorld2D::CreateBody(const BodySettings2D& s)
{
    if (!Finite(s.position) || !Finite(s.velocity) || !std::isfinite(s.rotation) || !std::isfinite(s.angularVelocity) ||
        !Nonnegative(s.linearDamping) || !Nonnegative(s.angularDamping) || !std::isfinite(s.gravityScale) ||
        !std::isfinite(s.mass) || s.mass <= 0 || s.type > BodyType2D::Kinematic) return {};
    std::unique_lock lock(m_state->mutex);
    auto body = std::make_unique<BodyRecord2D>();
    uint32_t index;
    if (m_state->freeBodies.empty())
    {
        index = static_cast<uint32_t>(m_state->bodies.size());
        m_state->bodies.emplace_back(); m_state->bodyGenerations.push_back(1);
    }
    else { index = m_state->freeBodies.back(); m_state->freeBodies.pop_back(); }
    body->handle = {m_state->world, index, m_state->bodyGenerations[index]}; body->settings = s;
    auto def = b2DefaultBodyDef();
    def.type = Native(s.type); def.position = b2ToPos(Native(s.position)); def.rotation = b2MakeRot(s.rotation);
    def.linearVelocity = Native(s.velocity); def.angularVelocity = s.angularVelocity;
    def.linearDamping = s.linearDamping; def.angularDamping = s.angularDamping; def.gravityScale = s.gravityScale;
    def.enableSleep = s.allowSleep; def.isAwake = s.startAwake; def.isBullet = s.bullet;
    def.motionLocks = {s.lockPositionX, s.lockPositionY, s.lockRotation}; def.userData = body.get();
    body->native = b2CreateBody(m_state->native, &def);
    if (B2_IS_NULL(body->native)) { m_state->freeBodies.push_back(index); return {}; }
    const auto handle = body->handle; m_state->bodies[index] = std::move(body);
    ++m_state->topologyRevision;
    return handle;
}
bool PhysicsWorld2D::UpdateDynamics(BodyHandle2D handle, const BodySettings2D& settings)
{
    if (settings.type>BodyType2D::Kinematic || !std::isfinite(settings.mass) || settings.mass<=0 ||
        !Nonnegative(settings.linearDamping) || !Nonnegative(settings.angularDamping) || !std::isfinite(settings.gravityScale)) return false;
    std::unique_lock lock(m_state->mutex);
    auto* body=m_state->Find(handle); if(!body) return false;
    if(body->settings.type!=settings.type) b2Body_SetType(body->native,Native(settings.type));
    b2Body_SetLinearDamping(body->native,settings.linearDamping); b2Body_SetAngularDamping(body->native,settings.angularDamping);
    b2Body_SetGravityScale(body->native,settings.gravityScale); b2Body_EnableSleeping(body->native,settings.allowSleep);
    b2Body_SetBullet(body->native,settings.bullet);
    b2Body_SetMotionLocks(body->native,{settings.lockPositionX,settings.lockPositionY,settings.lockRotation});
    body->settings=settings;
    b2Body_UpdateMassFromShapes(body->native); m_state->UpdateMass(*body); ++m_state->topologyRevision; return true;
}
bool PhysicsWorld2D::Remove(BodyHandle2D handle)
{
    std::unique_lock lock(m_state->mutex);
    auto* body = m_state->Find(handle); if (!body) return false;
    // Native body destruction releases all shapes at once. Invalidate engine
    // handles without repeatedly shifting the compound's ownership array.
    for (const auto shape : body->shapes)
    {
        m_state->shapes[shape.index].reset();
        if (++m_state->shapeGenerations[shape.index] == 0) ++m_state->shapeGenerations[shape.index];
        m_state->freeShapes.push_back(shape.index);
    }
    b2DestroyBody(body->native); m_state->bodies[handle.index].reset();
    if (++m_state->bodyGenerations[handle.index] == 0) ++m_state->bodyGenerations[handle.index];
    m_state->freeBodies.push_back(handle.index); ++m_state->topologyRevision; return true;
}
bool PhysicsWorld2D::Remove(ShapeHandle2D handle)
{ std::unique_lock lock(m_state->mutex); return m_state->RemoveShape(handle); }
ShapeHandle2D PhysicsWorld2D::AddBox(BodyHandle2D body, Maths::Vector2 half, const ShapeSettings2D& s)
{
    if (!Detail::ValidGeometryPoint2D(half) || half.x<1e-6f || half.y<1e-6f || !ValidShape(s)) return {};
    const auto polygon = b2MakeOffsetBox(half.x, half.y, Native(s.offset), b2MakeRot(s.rotation));
    for(int i=0;i<polygon.count;++i) if(!Detail::ValidGeometryPoint2D(Engine(polygon.vertices[i]))) return {};
    return m_state->AddShape(body, s, [&](b2BodyId id, const b2ShapeDef& def) { return b2CreatePolygonShape(id, &def, &polygon); });
}
ShapeHandle2D PhysicsWorld2D::AddCircle(BodyHandle2D body, float radius, const ShapeSettings2D& s)
{
    if (!std::isfinite(radius) || radius<1e-6f || radius>MaxGeometryCoordinate2D || !ValidShape(s)) return {};
    const b2Circle circle{Native(s.offset), radius};
    return m_state->AddShape(body, s, [&](b2BodyId id, const b2ShapeDef& def) { return b2CreateCircleShape(id, &def, &circle); });
}
ShapeHandle2D PhysicsWorld2D::AddCapsule(BodyHandle2D body, float radius, float halfHeight, const ShapeSettings2D& s)
{
    if (!std::isfinite(radius) || radius<1e-6f || radius>MaxGeometryCoordinate2D || !std::isfinite(halfHeight) || halfHeight<=0 || !ValidShape(s)) return {};
    const auto first=LocalPoint({0,-halfHeight},s),second=LocalPoint({0,halfHeight},s);
    if(!Detail::ValidGeometryEdge2D(first,second)) return {};
    const b2Capsule capsule{Native(first), Native(second), radius};
    return m_state->AddShape(body, s, [&](b2BodyId id, const b2ShapeDef& def) { return b2CreateCapsuleShape(id, &def, &capsule); });
}
ShapeHandle2D PhysicsWorld2D::AddConvexPolygon(BodyHandle2D body, std::span<const Maths::Vector2> vertices, const ShapeSettings2D& s)
{
    if (vertices.size() < 3 || vertices.size() > B2_MAX_POLYGON_VERTICES || !ValidShape(s) ||
        !std::all_of(vertices.begin(), vertices.end(), Finite)) return {};
    b2Vec2 points[B2_MAX_POLYGON_VERTICES];
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        const auto point=LocalPoint(vertices[i],s); if(!Detail::ValidGeometryPoint2D(point)) return {};
        points[i]=Native(point);
    }
    const auto hull = b2ComputeHull(points, static_cast<int>(vertices.size()));
    // A convex API must not silently fill a concave input's missing regions.
    if (hull.count != static_cast<int>(vertices.size()) || !b2ValidateHull(&hull)) return {};
    const auto polygon = b2MakePolygon(&hull, 0);
    return m_state->AddShape(body, s, [&](b2BodyId id, const b2ShapeDef& def) { return b2CreatePolygonShape(id, &def, &polygon); });
}
ShapeHandle2D PhysicsWorld2D::AddSegment(BodyHandle2D body,Maths::Vector2 first,Maths::Vector2 second,const ShapeSettings2D& s)
{
    if(!ValidShape(s)) return {};
    first=LocalPoint(first,s); second=LocalPoint(second,s);
    if(!Detail::ValidGeometryEdge2D(first,second)) return {};
    const b2Segment segment{Native(first),Native(second)};
    return m_state->AddShape(body,s,[&](b2BodyId id,const b2ShapeDef& def){return b2CreateSegmentShape(id,&def,&segment);});
}
ShapeHandle2D PhysicsWorld2D::AddChainSegment(BodyHandle2D body,std::span<const Maths::Vector2> points,const ShapeSettings2D& s)
{
    if(points.size()!=4 || !ValidShape(s) || s.trigger) return {};
    Maths::Vector2 transformed[4]; for(size_t i=0;i<4;++i) transformed[i]=LocalPoint(points[i],s);
    for(size_t i=0;i<3;++i) if(!Detail::ValidGeometryEdge2D(transformed[i],transformed[i+1])) return {};
    b2ChainSegment segment{}; segment.ghost1=Native(transformed[0]); segment.segment={Native(transformed[1]),Native(transformed[2])}; segment.ghost2=Native(transformed[3]);
    return m_state->AddShape(body,s,[&](b2BodyId id,const b2ShapeDef& def)
    {
        if(b2Body_GetType(id)!=b2_staticBody) return b2_nullShapeId;
        return b2CreateChainSegmentShape(id,&def,&segment);
    });
}
bool PhysicsWorld2D::IsValid(BodyHandle2D handle) const
{ std::shared_lock lock(m_state->mutex); return m_state->Find(handle)!=nullptr; }
bool PhysicsWorld2D::IsValid(ShapeHandle2D handle) const
{ std::shared_lock lock(m_state->mutex); return m_state->Find(handle)!=nullptr; }
BodyHandle2D PhysicsWorld2D::GetShapeBody(ShapeHandle2D handle) const
{ std::shared_lock lock(m_state->mutex); const auto* shape=m_state->Find(handle); return shape?shape->body:BodyHandle2D{}; }

bool PhysicsWorld2D::GetState(BodyHandle2D handle, BodyState2D& out) const
{
    std::shared_lock lock(m_state->mutex); const auto* body = m_state->Find(handle); if (!body) return false;
    out = ReadBodyState2D(*body, m_state->statistics.stepIndex);
    return true;
}
bool PhysicsWorld2D::SetTransform(BodyHandle2D handle, Maths::Vector2 position, float rotation, bool keepVelocity)
{
    if (!Finite(position) || !std::isfinite(rotation)) return false;
    std::unique_lock lock(m_state->mutex); auto* body = m_state->Find(handle); if (!body) return false;
    b2Body_SetTransform(body->native, b2ToPos(Native(position)), b2MakeRot(rotation));
    if (++body->poseRevision == 0) ++body->poseRevision;
    if (!keepVelocity) { b2Body_SetLinearVelocity(body->native, b2Vec2_zero); b2Body_SetAngularVelocity(body->native, 0); }
    return true;
}
bool PhysicsWorld2D::SetVelocity(BodyHandle2D handle, Maths::Vector2 linear, float angular)
{
    if (!Finite(linear) || !std::isfinite(angular)) return false;
    std::unique_lock lock(m_state->mutex); auto* body = m_state->Find(handle);
    if (!body || body->settings.type == BodyType2D::Static) return false;
    b2Body_SetLinearVelocity(body->native, Native(linear)); b2Body_SetAngularVelocity(body->native, angular); return true;
}
bool PhysicsWorld2D::AddForce(BodyHandle2D handle, Maths::Vector2 force, ForceMode2D mode)
{
    if (!Finite(force) || mode > ForceMode2D::VelocityChange) return false;
    std::unique_lock lock(m_state->mutex); auto* body = m_state->Find(handle);
    if (!body || body->settings.type != BodyType2D::Dynamic) return false;
    const float mass = b2Body_GetMass(body->native);
    auto value = Native(force);
    if (mode == ForceMode2D::Acceleration || mode == ForceMode2D::VelocityChange) { value.x *= mass; value.y *= mass; }
    if (!std::isfinite(value.x) || !std::isfinite(value.y)) return false;
    if (mode == ForceMode2D::Impulse || mode == ForceMode2D::VelocityChange) b2Body_ApplyLinearImpulseToCenter(body->native, value, true);
    else b2Body_ApplyForceToCenter(body->native, value, true);
    return true;
}
bool PhysicsWorld2D::AddForceAtPosition(BodyHandle2D handle, Maths::Vector2 force, Maths::Vector2 position, ForceMode2D mode)
{
    if (!Finite(force) || !Finite(position) || mode > ForceMode2D::VelocityChange) return false;
    std::unique_lock lock(m_state->mutex); auto* body = m_state->Find(handle);
    if (!body || body->settings.type != BodyType2D::Dynamic) return false;
    auto value = Native(force);
    if (mode == ForceMode2D::Acceleration || mode == ForceMode2D::VelocityChange)
    { const float mass = b2Body_GetMass(body->native); value.x *= mass; value.y *= mass; }
    if (!std::isfinite(value.x) || !std::isfinite(value.y)) return false;
    if (mode == ForceMode2D::Impulse || mode == ForceMode2D::VelocityChange) b2Body_ApplyLinearImpulse(body->native, value, b2ToPos(Native(position)), true);
    else b2Body_ApplyForce(body->native, value, b2ToPos(Native(position)), true);
    return true;
}
bool PhysicsWorld2D::AddTorque(BodyHandle2D handle, float torque, ForceMode2D mode)
{
    if (!std::isfinite(torque) || mode > ForceMode2D::VelocityChange) return false;
    std::unique_lock lock(m_state->mutex); auto* body = m_state->Find(handle);
    if (!body || body->settings.type != BodyType2D::Dynamic) return false;
    if (mode == ForceMode2D::Acceleration || mode == ForceMode2D::VelocityChange) torque *= b2Body_GetRotationalInertia(body->native);
    if (!std::isfinite(torque)) return false;
    if (mode == ForceMode2D::Impulse || mode == ForceMode2D::VelocityChange) b2Body_ApplyAngularImpulse(body->native, torque, true);
    else b2Body_ApplyTorque(body->native, torque, true);
    return true;
}
bool PhysicsWorld2D::SetAwake(BodyHandle2D handle, bool awake)
{
    std::unique_lock lock(m_state->mutex); auto* body = m_state->Find(handle);
    if (!body || body->settings.type != BodyType2D::Dynamic) return false;
    b2Body_SetAwake(body->native, awake); return true;
}
bool PhysicsWorld2D::SetGravity(Maths::Vector2 gravity)
{
    if (!Finite(gravity)) return false;
    std::unique_lock lock(m_state->mutex); m_state->settings.gravity = gravity;
    b2World_SetGravity(m_state->native, Native(gravity)); return true;
}
bool PhysicsWorld2D::SetSolverSubsteps(uint32_t subdivisions)
{
    if (subdivisions < 1 || subdivisions > 64) return false;
    std::unique_lock lock(m_state->mutex);
    m_state->settings.solverSubsteps = subdivisions;
    return true;
}
uint32_t PhysicsWorld2D::GetSolverSubsteps() const
{
    std::shared_lock lock(m_state->mutex);
    return m_state->settings.solverSubsteps;
}
void PhysicsWorld2D::Step(float delta)
{
    if (!std::isfinite(delta) || delta <= 0) return;
    std::unique_lock lock(m_state->mutex);
    const auto start = std::chrono::steady_clock::now();
    m_state->usedTasks = 0;
    b2World_Step(m_state->native, delta, static_cast<int>(m_state->settings.solverSubsteps));
    m_state->statistics.stepMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ++m_state->statistics.stepIndex;
}
PhysicsStatistics2D PhysicsWorld2D::GetStatistics() const
{
    std::shared_lock lock(m_state->mutex); auto statistics = m_state->statistics;
    const auto counters = b2World_GetCounters(m_state->native);
    statistics.bodyCount = counters.bodyCount; statistics.shapeCount = counters.shapeCount;
    statistics.contactCount = counters.contactCount; statistics.jointCount = counters.jointCount;
    for (const auto& body : m_state->bodies) if (body && body->settings.type == BodyType2D::Dynamic)
        if (b2Body_IsAwake(body->native)) ++statistics.activeBodyCount; else ++statistics.sleepingBodyCount;
    return statistics;
}

uint64_t PhysicsWorld2D::GetRevision() const noexcept { return m_state->mutex.Revision(); }

PhysicsState2D PhysicsWorld2D::CaptureState() const
{
    std::shared_lock lock(m_state->mutex);
    PhysicsState2D state; state.world=m_state->world; state.stepIndex=m_state->statistics.stepIndex;
    state.topologyRevision=m_state->topologyRevision;
    for(const auto& body:m_state->bodies) if(body) state.bodies.push_back(ReadBodyState2D(*body,m_state->statistics.stepIndex));
    return state;
}
bool PhysicsWorld2D::RestoreState(const PhysicsState2D& state, std::string& error)
{
    std::unique_lock lock(m_state->mutex);
    if(state.world!=m_state->world || state.topologyRevision!=m_state->topologyRevision ||
        state.bodies.size()!=static_cast<size_t>(b2World_GetCounters(m_state->native).bodyCount))
    { error="2D physics snapshot belongs to a different world or topology."; return false; }
    std::vector<bool> seen(m_state->bodies.size());
    for(const auto& saved:state.bodies)
    {
        if(!m_state->Find(saved.body) || seen[saved.body.index] || !Finite(saved.position) || !Finite(saved.velocity) ||
            !std::isfinite(saved.rotation) || !std::isfinite(saved.angularVelocity))
        { error="2D physics snapshot contains an invalid or duplicate body."; return false; }
        seen[saved.body.index]=true;
    }
    for(const auto& saved:state.bodies)
    {
        auto* body=m_state->Find(saved.body);
        if(body->settings.type==BodyType2D::Dynamic) b2Body_SetAwake(body->native,false);
        b2Body_SetTransform(body->native,b2ToPos(Native(saved.position)),b2MakeRot(saved.rotation));
        if (++body->poseRevision == 0) ++body->poseRevision;
        if(body->settings.type!=BodyType2D::Static)
        { b2Body_SetLinearVelocity(body->native,Native(saved.velocity)); b2Body_SetAngularVelocity(body->native,saved.angularVelocity); }
        if(body->settings.type==BodyType2D::Dynamic) b2Body_SetAwake(body->native,saved.awake);
    }
    m_state->statistics.stepIndex=state.stepIndex; m_state->statistics.stepMilliseconds=0;
    error.clear(); return true;
}

struct PhysicsWorld2D::ReadAccess::Impl
{
    std::shared_ptr<State> state;
    std::shared_lock<Detail::WorldAccessMutex> lock;
    QueryFilter2D filter;
    Impl(std::shared_ptr<State> s, QueryFilter2D f) : state(std::move(s)), lock(state->mutex), filter(std::move(f)) {}
    const ShapeRecord2D* Accept(b2ShapeId id) const
    {
        const auto* shape = static_cast<const ShapeRecord2D*>(b2Shape_GetUserData(id));
        if (!shape || !(shape->settings.layers & filter.layers) || (shape->settings.trigger && !filter.includeTriggers)) return nullptr;
        const auto* body = state->Find(shape->body);
        if (!body || (body->settings.type == BodyType2D::Static ? !filter.includeStatic : !filter.includeDynamic) ||
            std::find(filter.ignored.begin(), filter.ignored.end(), body->handle) != filter.ignored.end()) return nullptr;
        return shape;
    }
    QueryHit2D Hit(const ShapeRecord2D& s, b2Vec2 point, b2Vec2 normal, float fraction, float distance) const
    { return {s.body, s.handle, s.settings.owner, Engine(point), Engine(normal), distance * fraction, fraction,
        normal.x * normal.x + normal.y * normal.y > 1e-12f,s.settings.subShapeId}; }
};
PhysicsWorld2D::ReadAccess::ReadAccess(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
PhysicsWorld2D::ReadAccess::~ReadAccess() = default;
std::unique_ptr<PhysicsWorld2D::ReadAccess> PhysicsWorld2D::Read(const QueryFilter2D& filter) const
{ return std::unique_ptr<ReadAccess>(new ReadAccess(std::make_unique<ReadAccess::Impl>(m_state, filter))); }
bool PhysicsWorld2D::ReadAccess::Raycast(Maths::Vector2 origin, Maths::Vector2 direction, float distance, QueryHit2D& hit) const
{
    b2Vec2 translation;
    if (!CastVector(origin, direction, distance, translation)) return false;
    struct Context { const Impl* impl; QueryHit2D hit; float distance; bool found = false; } ctx{m_impl.get(), {}, distance};
    const auto callback = [](b2ShapeId id, b2Pos point, b2Vec2 normal, float fraction, void* data) -> float
    {
        auto& c = *static_cast<Context*>(data); const auto* shape = c.impl->Accept(id); if (!shape) return -1;
        c.hit = c.impl->Hit(*shape, point, normal, fraction, c.distance); c.found = true; return fraction;
    };
    auto filter = b2DefaultQueryFilter(); filter.categoryBits = kQueryCategory; filter.maskBits = m_impl->filter.layers;
    b2World_CastRay(m_impl->state->native, b2ToPos(Native(origin)), translation, filter, callback, &ctx);
    if (ctx.found) hit = ctx.hit;
    return ctx.found;
}
std::vector<QueryHit2D> PhysicsWorld2D::ReadAccess::RaycastAll(Maths::Vector2 origin, Maths::Vector2 direction, float distance) const
{
    std::vector<QueryHit2D> hits; b2Vec2 translation;
    if (!CastVector(origin, direction, distance, translation)) return hits;
    struct Context { const Impl* impl; std::vector<QueryHit2D>* hits; float distance; } ctx{m_impl.get(), &hits, distance};
    const auto callback = [](b2ShapeId id, b2Pos point, b2Vec2 normal, float fraction, void* data) -> float
    {
        auto& c = *static_cast<Context*>(data); const auto* shape = c.impl->Accept(id);
        if (shape) c.hits->push_back(c.impl->Hit(*shape, point, normal, fraction, c.distance)); return 1;
    };
    auto filter = b2DefaultQueryFilter(); filter.categoryBits = kQueryCategory; filter.maskBits = m_impl->filter.layers;
    b2World_CastRay(m_impl->state->native, b2ToPos(Native(origin)), translation, filter, callback, &ctx);
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; }); return hits;
}
std::vector<QueryHit2D> PhysicsWorld2D::ReadAccess::OverlapCircle(Maths::Vector2 center, float radius) const
{
    std::vector<QueryHit2D> hits;
    if (!Finite(center) || !std::isfinite(radius) || radius <= 0) return hits;
    struct Context { const Impl* impl; std::vector<QueryHit2D>* hits; } ctx{m_impl.get(), &hits};
    const auto callback = [](b2ShapeId id, void* data) -> bool
    {
        auto& c = *static_cast<Context*>(data); const auto* shape = c.impl->Accept(id);
        if (shape) c.hits->push_back(c.impl->Hit(*shape, {}, {}, 0, 0)); return true;
    };
    const b2Vec2 point{}; const auto proxy = b2MakeProxy(&point, 1, radius);
    auto filter = b2DefaultQueryFilter(); filter.categoryBits = kQueryCategory; filter.maskBits = m_impl->filter.layers;
    b2World_OverlapShape(m_impl->state->native, b2ToPos(Native(center)), &proxy, filter, callback, &ctx); return hits;
}
QueryBufferResult PhysicsWorld2D::ReadAccess::RaycastNonAlloc(Maths::Vector2 origin, Maths::Vector2 direction, float distance, std::span<QueryHit2D> output) const
{
    b2Vec2 translation;
    if (!CastVector(origin,direction,distance,translation)) return {};
    Detail::BoundedQueryResults<QueryHit2D> results(output);
    struct Context { const Impl* impl; Detail::BoundedQueryResults<QueryHit2D>* results; float distance; } ctx{m_impl.get(),&results,distance};
    const auto callback=[](b2ShapeId id,b2Pos point,b2Vec2 normal,float fraction,void* data)->float
    {
        auto& c=*static_cast<Context*>(data);
        if(const auto* shape=c.impl->Accept(id)) c.results->Add(c.impl->Hit(*shape,point,normal,fraction,c.distance));
        return 1;
    };
    auto filter=b2DefaultQueryFilter(); filter.categoryBits=kQueryCategory; filter.maskBits=m_impl->filter.layers;
    b2World_CastRay(m_impl->state->native,b2ToPos(Native(origin)),translation,filter,callback,&ctx);
    return results.Finish();
}
QueryBufferResult PhysicsWorld2D::ReadAccess::OverlapCircleNonAlloc(Maths::Vector2 center, float radius, std::span<QueryHit2D> output) const
{
    if(!Finite(center) || !std::isfinite(radius) || radius<=0) return {};
    Detail::BoundedQueryResults<QueryHit2D> results(output);
    struct Context { const Impl* impl; Detail::BoundedQueryResults<QueryHit2D>* results; } ctx{m_impl.get(),&results};
    const auto callback=[](b2ShapeId id,void* data)->bool
    {
        auto& c=*static_cast<Context*>(data);
        if(const auto* shape=c.impl->Accept(id)) c.results->Add(c.impl->Hit(*shape,{},{},0,0));
        return true;
    };
    const b2Vec2 point{}; const auto proxy=b2MakeProxy(&point,1,radius);
    auto filter=b2DefaultQueryFilter(); filter.categoryBits=kQueryCategory; filter.maskBits=m_impl->filter.layers;
    b2World_OverlapShape(m_impl->state->native,b2ToPos(Native(center)),&proxy,filter,callback,&ctx);
    return results.Finish();
}

bool PhysicsWorld2D::ReadAccess::CircleCast(Maths::Vector2 origin, float radius, Maths::Vector2 direction, float distance, QueryHit2D& hit) const
{
    b2Vec2 translation;
    if (!std::isfinite(radius) || radius <= 0 || !CastVector(origin, direction, distance, translation)) return false;
    struct Context { const Impl* impl; QueryHit2D hit; float distance; bool found = false; } ctx{m_impl.get(), {}, distance};
    const auto callback = [](b2ShapeId id, b2Pos point, b2Vec2 normal, float fraction, void* data) -> float
    {
        auto& c = *static_cast<Context*>(data); const auto* shape = c.impl->Accept(id); if (!shape) return -1;
        c.hit = c.impl->Hit(*shape, point, normal, fraction, c.distance); c.found = true; return fraction;
    };
    const b2Vec2 point{}; const auto proxy = b2MakeProxy(&point, 1, radius);
    auto filter = b2DefaultQueryFilter(); filter.categoryBits = kQueryCategory; filter.maskBits = m_impl->filter.layers;
    b2World_CastShape(m_impl->state->native, b2ToPos(Native(origin)), &proxy, translation, filter, callback, &ctx);
    if (ctx.found) hit = ctx.hit;
    return ctx.found;
}
}
