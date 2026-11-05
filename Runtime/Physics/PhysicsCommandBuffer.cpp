#include "PhysicsCommandBuffer.h"
#include <atomic>
#include <mutex>
#include <variant>
#include <type_traits>
#include <utility>
#include <set>

namespace NLS::Physics
{
namespace
{
std::atomic<uint64_t> nextBufferIdentity{1};
struct Create3D { BodySettings settings; };
struct Create2D { BodySettings2D settings; };
struct Attach3D { PhysicsBodyReference body; PhysicsShapeReference previous; CollisionGeometry3D geometry; ShapeSettings settings; };
struct Attach2D { PhysicsBodyReference body; PhysicsShapeReference previous; CollisionGeometry2D geometry; ShapeSettings2D settings; };
struct ReplaceGroup3D { PhysicsBodyReference body; std::vector<PhysicsShapeReference> previous; std::vector<PhysicsShapeDescription3D> shapes; };
struct ReplaceGroup2D { PhysicsBodyReference body; std::vector<PhysicsShapeReference> previous; std::vector<PhysicsShapeDescription2D> shapes; };
struct Dynamics3D { PhysicsBodyReference body; BodySettings settings; };
struct Dynamics2D { PhysicsBodyReference body; BodySettings2D settings; };
struct RemoveBodyCommand { PhysicsBodyReference body; };
struct RemoveShapeCommand { PhysicsShapeReference shape; };
struct Gravity3D { Maths::Vector3 value; };
struct Gravity2D { Maths::Vector2 value; };
struct SolverSubsteps2D { uint32_t value; };
using Command = std::variant<Create3D, Create2D, Attach3D, Attach2D, Dynamics3D, Dynamics2D, RemoveBodyCommand, RemoveShapeCommand,
    Gravity3D, Gravity2D, SolverSubsteps2D, ReplaceGroup3D, ReplaceGroup2D>;

PhysicsCommandResult Failure(PhysicsCommandError error)
{ PhysicsCommandResult result; result.status=PhysicsCommandStatus::Failed; result.error=error; return result; }

template<class World>
bool ApplySolverSubsteps(World& world, uint32_t subdivisions)
{
    if constexpr (std::is_same_v<World,PhysicsWorld2D>) return world.SetSolverSubsteps(subdivisions);
    else return false;
}

ShapeHandle Attach(PhysicsWorld& world, BodyHandle body, const CollisionGeometry3D& geometry, const ShapeSettings& settings)
{
    switch (geometry.type)
    {
    case CollisionGeometryType3D::Box: return world.AttachBox(body,geometry.halfExtents,settings);
    case CollisionGeometryType3D::Sphere: return world.AttachSphere(body,geometry.radius,settings);
    case CollisionGeometryType3D::Capsule: return world.AttachCapsule(body,geometry.radius,geometry.halfHeight,settings);
    case CollisionGeometryType3D::Cylinder: return world.AttachCylinder(body,geometry.radius,geometry.halfHeight,settings);
    case CollisionGeometryType3D::ConvexMesh: return world.AttachConvexMesh(body,geometry.vertices,settings);
    case CollisionGeometryType3D::TriangleMesh: return world.AttachTriangleMesh(body,geometry.vertices,geometry.indices,settings);
    case CollisionGeometryType3D::HeightField: return world.AttachHeightField(body,geometry.rows,geometry.columns,geometry.heights,geometry.rowSpacing,geometry.columnSpacing,settings);
    case CollisionGeometryType3D::CookedMesh: return world.AttachCollisionMesh(body,geometry.cookedMesh,settings,geometry.meshScale);
    }
    return {};
}
ShapeHandle2D Attach(PhysicsWorld2D& world, BodyHandle2D body, const CollisionGeometry2D& geometry, const ShapeSettings2D& settings)
{
    switch (geometry.type)
    {
    case CollisionGeometryType2D::Box: return world.AddBox(body,geometry.halfExtents,settings);
    case CollisionGeometryType2D::Circle: return world.AddCircle(body,geometry.radius,settings);
    case CollisionGeometryType2D::Capsule: return world.AddCapsule(body,geometry.radius,geometry.halfHeight,settings);
    case CollisionGeometryType2D::ConvexPolygon: return world.AddConvexPolygon(body,geometry.vertices,settings);
    case CollisionGeometryType2D::Segment:
        return geometry.vertices.size()==2?world.AddSegment(body,geometry.vertices[0],geometry.vertices[1],settings):ShapeHandle2D{};
    case CollisionGeometryType2D::ChainSegment: return world.AddChainSegment(body,geometry.vertices,settings);
    }
    return {};
}
}

struct PhysicsCommandBuffer::State
{
    std::weak_ptr<PhysicsWorld> world3D;
    std::weak_ptr<PhysicsWorld2D> world2D;
    bool twoDimensional = false, committing = false;
    uint64_t identity = nextBufferIdentity.fetch_add(1), generation = 1;
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<const Command>> commands;
    std::vector<PhysicsCommandResult> results;
    size_t committed = 0;

    PhysicsCommandTicket Record(Command value)
    {
        auto snapshot=std::make_shared<const Command>(std::move(value));
        std::lock_guard lock(mutex);
        commands.push_back(std::move(snapshot));
        try { results.push_back({PhysicsCommandStatus::Pending}); }
        catch (...) { commands.pop_back(); throw; }
        return {identity,generation,commands.size()-1};
    }
    const PhysicsCommandResult* Resolve(PhysicsCommandTicket ticket, const std::vector<PhysicsCommandResult>& values, size_t before) const
    {
        if (ticket.buffer!=identity || ticket.generation!=generation || ticket.index>=before ||
            ticket.index>=values.size() || values[ticket.index].status!=PhysicsCommandStatus::Applied) return nullptr;
        return &values[ticket.index];
    }
    template<class Handle, class Reference>
    Handle ResolveHandle(const Reference& reference, const std::vector<PhysicsCommandResult>& values, size_t before) const
    {
        if (reference.command.IsValid())
        {
            const auto* result=Resolve(reference.command,values,before); if (!result) return {};
            if constexpr (std::is_same_v<Handle,BodyHandle>) return result->body;
            else if constexpr (std::is_same_v<Handle,BodyHandle2D>) return result->body2D;
            else if constexpr (std::is_same_v<Handle,ShapeHandle>) return result->shape;
            else return result->shape2D;
        }
        if constexpr (std::is_same_v<Handle,BodyHandle>) return reference.body;
        else if constexpr (std::is_same_v<Handle,BodyHandle2D>) return reference.body2D;
        else if constexpr (std::is_same_v<Handle,ShapeHandle>) return reference.shape;
        else return reference.shape2D;
    }
    template<class World, class Body, class Shape>
    PhysicsCommandResult Apply(World& world, const Command& command, const std::vector<PhysicsCommandResult>& values, size_t index) const
    {
        return std::visit([&](const auto& value) -> PhysicsCommandResult
        {
            using Value=std::decay_t<decltype(value)>;
            PhysicsCommandResult result; result.status=PhysicsCommandStatus::Applied;
            const auto saveBody=[&](Body handle)
            {
                if constexpr (std::is_same_v<Body,BodyHandle>) result.body=handle;
                else result.body2D=handle;
                if (!handle.IsValid()) result=Failure(PhysicsCommandError::BackendRejected);
            };
            const auto saveShape=[&](Shape handle)
            {
                if constexpr (std::is_same_v<Shape,ShapeHandle>) result.shape=handle;
                else result.shape2D=handle;
                if (!handle.IsValid()) result=Failure(PhysicsCommandError::BackendRejected);
            };
            if constexpr (std::is_same_v<Value,RemoveBodyCommand> || std::is_same_v<Value,RemoveShapeCommand>)
            {
                bool removed=false;
                if constexpr (std::is_same_v<Value,RemoveBodyCommand>)
                {
                    const auto handle=ResolveHandle<Body>(value.body,values,index);
                    if (!handle.IsValid()) return Failure(PhysicsCommandError::InvalidReference);
                    removed=world.Remove(handle);
                }
                else
                {
                    const auto handle=ResolveHandle<Shape>(value.shape,values,index);
                    if (!handle.IsValid()) return Failure(PhysicsCommandError::InvalidReference);
                    removed=world.Remove(handle);
                }
                if (!removed) { result.status=PhysicsCommandStatus::Skipped; result.error=PhysicsCommandError::StaleOrForeignHandle; }
            }
            else if constexpr ((std::is_same_v<World,PhysicsWorld> && std::is_same_v<Value,Gravity3D>) ||
                (std::is_same_v<World,PhysicsWorld2D> && std::is_same_v<Value,Gravity2D>))
            {
                if (!world.SetGravity(value.value)) return Failure(PhysicsCommandError::BackendRejected);
            }
            else if constexpr (std::is_same_v<World,PhysicsWorld2D> && std::is_same_v<Value,SolverSubsteps2D>)
            {
                if (!ApplySolverSubsteps(world,value.value)) return Failure(PhysicsCommandError::BackendRejected);
            }
            else if constexpr ((std::is_same_v<World,PhysicsWorld> && std::is_same_v<Value,ReplaceGroup3D>) ||
                (std::is_same_v<World,PhysicsWorld2D> && std::is_same_v<Value,ReplaceGroup2D>))
            {
                constexpr auto limit=std::is_same_v<World,PhysicsWorld>?MaxCompoundShapes3D:MaxCompoundShapes2D;
                const auto body=ResolveHandle<Body>(value.body,values,index);
                if(!world.IsValid(body) || value.shapes.size()>limit || value.previous.size()>limit)
                    return Failure(PhysicsCommandError::InvalidReference);
                std::set<Shape> previous;
                for(const auto& reference:value.previous)
                {
                    const auto shape=ResolveHandle<Shape>(reference,values,index);
                    if(world.GetShapeBody(shape)!=body || !previous.insert(shape).second) return Failure(PhysicsCommandError::InvalidReference);
                }
                std::set<std::pair<uint64_t,uint32_t>> identities;
                for(const auto& shape:value.shapes)
                    if(shape.settings.subShapeId && !identities.emplace(shape.settings.owner,shape.settings.subShapeId).second)
                        return Failure(PhysicsCommandError::InvalidReference);
                std::vector<Shape> attached; attached.reserve(value.shapes.size());
                try
                {
                    for(const auto& description:value.shapes)
                    {
                        const auto shape=Attach(world,body,description.geometry,description.settings);
                        if(!shape.IsValid())
                        { for(const auto handle:attached) world.Remove(handle); return Failure(PhysicsCommandError::BackendRejected); }
                        attached.push_back(shape);
                    }
                }
                catch(...)
                { for(const auto handle:attached) world.Remove(handle); throw; }
                for(const auto shape:previous) world.Remove(shape);
                if constexpr(std::is_same_v<World,PhysicsWorld>)
                { result.body=body; result.shapes=std::move(attached); if(!result.shapes.empty()) result.shape=result.shapes.front(); }
                else
                { result.body2D=body; result.shapes2D=std::move(attached); if(!result.shapes2D.empty()) result.shape2D=result.shapes2D.front(); }
            }
            else if constexpr ((std::is_same_v<World,PhysicsWorld> && (std::is_same_v<Value,Create3D> || std::is_same_v<Value,Attach3D> || std::is_same_v<Value,Dynamics3D>)) ||
                (std::is_same_v<World,PhysicsWorld2D> && (std::is_same_v<Value,Create2D> || std::is_same_v<Value,Attach2D> || std::is_same_v<Value,Dynamics2D>)))
            {
                if constexpr (std::is_same_v<Value,Create3D> || std::is_same_v<Value,Create2D>) saveBody(world.CreateBody(value.settings));
                else
                {
                    const auto body=ResolveHandle<Body>(value.body,values,index);
                    if (!body.IsValid()) return Failure(PhysicsCommandError::InvalidReference);
                    if constexpr (std::is_same_v<Value,Dynamics3D> || std::is_same_v<Value,Dynamics2D>)
                    {
                        if (!world.UpdateDynamics(body,value.settings)) return Failure(PhysicsCommandError::BackendRejected);
                        saveBody(body);
                    }
                    else
                    {
                        const bool replacing=value.previous.command.IsValid() || value.previous.shape.IsValid() || value.previous.shape2D.IsValid();
                        const auto previous=ResolveHandle<Shape>(value.previous,values,index);
                        if (replacing && (!previous.IsValid() || world.GetShapeBody(previous)!=body)) return Failure(PhysicsCommandError::InvalidReference);
                        const auto attached=Attach(world,body,value.geometry,value.settings);
                        saveShape(attached);
                        if (attached.IsValid() && replacing) world.Remove(previous);
                    }
                }
            }
            else return Failure(PhysicsCommandError::WrongDimension);
            return result;
        },command);
    }
};

PhysicsCommandBuffer::PhysicsCommandBuffer(const std::shared_ptr<PhysicsWorld>& world) : m_state(std::make_unique<State>()) { m_state->world3D=world; }
PhysicsCommandBuffer::PhysicsCommandBuffer(const std::shared_ptr<PhysicsWorld2D>& world) : m_state(std::make_unique<State>()) { m_state->world2D=world; m_state->twoDimensional=true; }
PhysicsCommandBuffer::~PhysicsCommandBuffer() = default;
bool PhysicsCommandBuffer::Matches(const std::shared_ptr<PhysicsWorld>& world) const { return world && !m_state->twoDimensional && m_state->world3D.lock()==world; }
bool PhysicsCommandBuffer::Matches(const std::shared_ptr<PhysicsWorld2D>& world) const { return world && m_state->twoDimensional && m_state->world2D.lock()==world; }
PhysicsCommandTicket PhysicsCommandBuffer::CreateBody(const BodySettings& value) { return m_state->Record(Create3D{value}); }
PhysicsCommandTicket PhysicsCommandBuffer::CreateBody(const BodySettings2D& value) { return m_state->Record(Create2D{value}); }
PhysicsCommandTicket PhysicsCommandBuffer::AttachShape(PhysicsBodyReference body, CollisionGeometry3D geometry, const ShapeSettings& settings) { return ReplaceShape(body,{},std::move(geometry),settings); }
PhysicsCommandTicket PhysicsCommandBuffer::AttachShape(PhysicsBodyReference body, CollisionGeometry2D geometry, const ShapeSettings2D& settings) { return ReplaceShape(body,{},std::move(geometry),settings); }
PhysicsCommandTicket PhysicsCommandBuffer::ReplaceShape(PhysicsBodyReference body, PhysicsShapeReference previous, CollisionGeometry3D geometry, const ShapeSettings& settings) { return m_state->Record(Attach3D{body,previous,std::move(geometry),settings}); }
PhysicsCommandTicket PhysicsCommandBuffer::ReplaceShape(PhysicsBodyReference body, PhysicsShapeReference previous, CollisionGeometry2D geometry, const ShapeSettings2D& settings) { return m_state->Record(Attach2D{body,previous,std::move(geometry),settings}); }
PhysicsCommandTicket PhysicsCommandBuffer::ReplaceShapes(PhysicsBodyReference body,std::vector<PhysicsShapeReference> previous,std::vector<PhysicsShapeDescription3D> shapes)
{ return m_state->Record(ReplaceGroup3D{body,std::move(previous),std::move(shapes)}); }
PhysicsCommandTicket PhysicsCommandBuffer::ReplaceShapes2D(PhysicsBodyReference body,std::vector<PhysicsShapeReference> previous,std::vector<PhysicsShapeDescription2D> shapes)
{ return m_state->Record(ReplaceGroup2D{body,std::move(previous),std::move(shapes)}); }
PhysicsCommandTicket PhysicsCommandBuffer::UpdateDynamics(PhysicsBodyReference body, const BodySettings& settings) { return m_state->Record(Dynamics3D{body,settings}); }
PhysicsCommandTicket PhysicsCommandBuffer::UpdateDynamics(PhysicsBodyReference body, const BodySettings2D& settings) { return m_state->Record(Dynamics2D{body,settings}); }
PhysicsCommandTicket PhysicsCommandBuffer::RemoveBody(PhysicsBodyReference body) { return m_state->Record(RemoveBodyCommand{body}); }
PhysicsCommandTicket PhysicsCommandBuffer::RemoveShape(PhysicsShapeReference shape) { return m_state->Record(RemoveShapeCommand{shape}); }
PhysicsCommandTicket PhysicsCommandBuffer::SetGravity(Maths::Vector3 gravity) { return m_state->Record(Gravity3D{gravity}); }
PhysicsCommandTicket PhysicsCommandBuffer::SetGravity(Maths::Vector2 gravity) { return m_state->Record(Gravity2D{gravity}); }
PhysicsCommandTicket PhysicsCommandBuffer::SetSolverSubsteps(uint32_t subdivisions) { return m_state->Record(SolverSubsteps2D{subdivisions}); }

PhysicsCommandCommitReport PhysicsCommandBuffer::Commit()
{
    auto& state=*m_state;
    std::vector<std::shared_ptr<const Command>> snapshot;
    std::vector<PhysicsCommandResult> results;
    size_t begin=0, end=0;
    {
        std::lock_guard lock(state.mutex);
        if (state.committing) { PhysicsCommandCommitReport report; report.busy=true; report.pending=state.commands.size()-state.committed; return report; }
        begin=state.committed; end=state.commands.size();
        if (begin==end) return {};
        snapshot.assign(state.commands.begin()+begin,state.commands.end()); results=state.results;
        state.committing=true;
    }
    const auto execute=[&](auto world, auto bodyType, auto shapeType)
    {
        if (!world)
        { for (size_t i=begin;i<end;++i) results[i]=Failure(PhysicsCommandError::ExpiredWorld); return; }
        const auto batch=world->BeginMutationBatch();
        for (size_t i=begin;i<end;++i)
        {
            try { results[i]=state.Apply<std::remove_reference_t<decltype(*world)>,decltype(bodyType),decltype(shapeType)>(*world,*snapshot[i-begin],results,i); }
            catch (...) { results[i]=Failure(PhysicsCommandError::UnexpectedException); }
        }
    };
    try
    {
        if (state.twoDimensional) execute(state.world2D.lock(),BodyHandle2D{},ShapeHandle2D{});
        else execute(state.world3D.lock(),BodyHandle{},ShapeHandle{});
    }
    catch (...) { for (size_t i=begin;i<end;++i) if (results[i].status==PhysicsCommandStatus::Pending) results[i]=Failure(PhysicsCommandError::UnexpectedException); }
    PhysicsCommandCommitReport report; report.processed=end-begin;
    {
        std::lock_guard lock(state.mutex);
        for (size_t i=begin;i<end;++i)
        {
            state.results[i]=results[i]; state.commands[i].reset();
            if (results[i].status==PhysicsCommandStatus::Applied) ++report.applied;
            else if (results[i].status==PhysicsCommandStatus::Skipped) ++report.skipped;
            else ++report.failed;
        }
        state.committed=end; state.committing=false; report.pending=state.commands.size()-end;
    }
    return report;
}
PhysicsCommandResult PhysicsCommandBuffer::GetResult(PhysicsCommandTicket ticket) const
{
    std::lock_guard lock(m_state->mutex);
    if (ticket.buffer!=m_state->identity || ticket.generation!=m_state->generation || ticket.index>=m_state->results.size()) return {};
    return m_state->results[ticket.index];
}
size_t PhysicsCommandBuffer::GetPendingCount() const { std::lock_guard lock(m_state->mutex); return m_state->commands.size()-m_state->committed; }
size_t PhysicsCommandBuffer::GetResultCount() const { std::lock_guard lock(m_state->mutex); return m_state->results.size(); }
bool PhysicsCommandBuffer::IsCommitting() const { std::lock_guard lock(m_state->mutex); return m_state->committing; }
bool PhysicsCommandBuffer::Reset()
{
    std::lock_guard lock(m_state->mutex);
    if (m_state->committing) return false;
    m_state->commands.clear(); m_state->results.clear(); m_state->committed=0;
    if (++m_state->generation==0) ++m_state->generation;
    return true;
}
}
