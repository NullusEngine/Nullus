#include "PhysicsCollider2DComponent.h"
#include "Rigidbody2DComponent.h"
#include "Physics2DTransform.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include <Physics/PhysicsCommandBuffer.h>
#include <Physics/Geometry2D.h>
#include <algorithm>
#include <cmath>

namespace NLS::Engine::Components
{
using namespace Physics2DDetail;
PhysicsCollider2DComponent::~PhysicsCollider2DComponent() { Release(); }
void PhysicsCollider2DComponent::OnDestroy() { Release(); }
void PhysicsCollider2DComponent::OnDisable() { Release(); }
void PhysicsCollider2DComponent::Release()
{ ReleaseWithContext(nullptr); }
void PhysicsCollider2DComponent::ReleaseToScene(SceneSystem::Scene& scene)
{ ReleaseWithContext(&scene); }
void PhysicsCollider2DComponent::ReleaseWithContext(SceneSystem::Scene* context)
{
    if (auto world = m_world.lock())
    {
        auto* scene=context?context:(gameobject()?gameobject()->GetScene():nullptr);
        if (m_ownsBody) { if (scene) scene->QueuePhysicsRemoval(world,m_body); else world->Remove(m_body); }
        else for(const auto shape:m_shapes) { if(scene) scene->QueuePhysicsRemoval(world,shape); else world->Remove(shape); }
    }
    m_shape = {}; m_shapes.clear(); m_body = {}; m_world.reset(); m_ownsBody = false; m_dirty = true;
}
void PhysicsCollider2DComponent::Synchronize()
{
    auto* owner = gameobject(); auto* scene = owner ? owner->GetScene() : nullptr;
    if (!scene || !IsActiveAndEnabled()) { Release(); return; }
    if (scene->IsPhysicsMutationDeferred() || (scene->IsPlaying() && !scene->IsPhysicsSynchronizing())) return;
    Frame frame; if (!Capture(*owner->GetTransform(), frame, m_error)) return;
    std::shared_ptr<const Assets::PhysicsMaterialData> materialData;
    if (!physicsMaterial.IsNull())
    {
        Serialize::ObjectIdentifier reference;
        if (Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(physicsMaterial.GetInstanceID(),reference) && reference.IsAsset())
        {
            auto asset=Assets::LoadPhysicsMaterial(reference,m_error); if(!asset) return;
            m_materialOwner=std::move(asset); materialData=m_materialOwner->Capture();
        }
        else if(const auto* asset=physicsMaterial.Get()) materialData=asset->Capture();
        if(!materialData) { m_error="Physics material reference is unresolved."; return; }
    }
    if(materialData!=m_materialData) { m_materialData=std::move(materialData); m_dirty=true; }
    if(physicsMaterial.IsNull()) m_materialOwner.reset();
    Rigidbody2DComponent* rigidbody = nullptr;
    for (auto* object = owner; object; object = object->GetParent())
        if (auto* candidate = object->GetComponent<Rigidbody2DComponent>())
        { if (candidate->IsActiveAndEnabled()) rigidbody = candidate; break; }
    const auto& world = scene->GetPhysicsWorld2D(); if (!world) { Release(); return; }
    const auto mutation=world->BeginMutationBatch();
    Physics::BodyHandle2D body;
    Frame root = frame;
    if (rigidbody)
    {
        rigidbody->Synchronize(); body = rigidbody->GetBodyHandle();
        if (!body.IsValid() || !Capture(*rigidbody->gameobject()->GetTransform(), root, m_error) ||
            !Capture(*owner->GetTransform(), frame, m_error)) { Release(); return; }
    }
    else
    {
        if (!m_ownsBody || m_world.lock() != world || !world->IsValid(m_body))
        {
            Release(); Physics::BodySettings2D settings; settings.position = frame.position; settings.rotation = frame.angle;
            Physics::PhysicsCommandBuffer commands(world);
            const auto creation=commands.CreateBody(settings); commands.Commit();
            m_body=commands.GetResult(creation).body2D; m_ownsBody=true; m_world=world;
        }
        body = m_body;
        if (!body.IsValid()) { m_error = "Cannot create the static 2D collider body."; return; }
    }
    const float angle = localRotation * Pi / 180;
    const auto axisX = frame.xAxis * std::cos(angle) + frame.yAxis * std::sin(angle);
    const auto axisY = frame.yAxis * std::cos(angle) - frame.xAxis * std::sin(angle);
    const float scaleX = std::hypot(axisX.x, axisX.y), scaleY = std::hypot(axisY.x, axisY.y);
    const bool complex=shape==EColliderShape::POLYGON || shape==EColliderShape::EDGE || shape==EColliderShape::CHAIN;
    if (!complex && shape != EColliderShape::CIRCLE && std::abs(axisX.x * axisY.x + axisX.y * axisY.y) > 1e-4f * scaleX * scaleY)
    { m_error = "Collider rotation under nonuniform scale creates unsupported shear."; return; }
    Physics::ShapeSettings2D settings;
    settings.offset = Rotate(frame.position + frame.xAxis * center.x + frame.yAxis * center.y - root.position, -root.angle);
    settings.rotation = std::atan2(axisX.y, axisX.x) - root.angle;
    settings.layers = layers; settings.collisionMask = collisionMask; settings.trigger = trigger;
    settings.density = density; settings.friction = friction; settings.restitution = restitution;
    if(m_materialData) { settings.friction=m_materialData->friction2D; settings.restitution=m_materialData->restitution2D; }
    settings.owner = static_cast<uint64_t>(GetInstanceID());
    const Maths::Vector2 halfSize{size.x * scaleX * 0.5f, size.y * scaleY * 0.5f};
    const float scaledRadius = radius * (shape == EColliderShape::CIRCLE ? (std::max)(scaleX, scaleY) : scaleX);
    const float scaledHeight = halfHeight * scaleY;
    const auto localAxisX=Rotate(axisX,-root.angle),localAxisY=Rotate(axisY,-root.angle);
    const auto updateStaticPose=[&]
    {
        if(!rigidbody)
        {
            Physics::BodyState2D state;
            if(world->GetState(body,state) && (!Same(state.position,frame.position) || !SameAngle(state.rotation,frame.angle)))
            { world->SetTransform(body,frame.position,frame.angle); scene->NotifyPhysicsChanged(); }
        }
    };
    if (!m_shapes.empty() && m_world.lock() == world && m_body == body && IsQueryReady() && !m_dirty &&
        Same(settings.offset, m_syncedCenter) && Same(halfSize, m_syncedSize) &&
        SameAngle(settings.rotation, m_syncedRotation) &&
        Same(localAxisX,m_syncedAxisX) && Same(localAxisY,m_syncedAxisY) &&
        std::abs(scaledRadius - m_syncedRadius) < 1e-6f && std::abs(scaledHeight - m_syncedHeight) < 1e-6f)
    { updateStaticPose(); m_error.clear(); return; }
    // Build a replacement before removing the last valid shape.
    Physics::CollisionGeometry2D geometry;
    std::vector<Physics::PhysicsShapeDescription2D> descriptions;
    switch (shape)
    {
    case EColliderShape::BOX: geometry.halfExtents=halfSize; break;
    case EColliderShape::CIRCLE: geometry.type=Physics::CollisionGeometryType2D::Circle; geometry.radius=scaledRadius; break;
    case EColliderShape::CAPSULE: geometry.type=Physics::CollisionGeometryType2D::Capsule; geometry.radius=scaledRadius; geometry.halfHeight=scaledHeight; break;
    case EColliderShape::POLYGON:
    case EColliderShape::EDGE:
    case EColliderShape::CHAIN:
    {
        std::vector<Maths::Vector2> points; points.reserve(vertices.size());
        for(const auto point:vertices) points.push_back(localAxisX*point.x+localAxisY*point.y);
        auto baked=settings; baked.rotation=0;
        if(shape==EColliderShape::POLYGON)
        { if(!Physics::BuildPolygon2D(points,baked,descriptions,m_error)) return; }
        else if(shape==EColliderShape::CHAIN)
        {
            // Preserve the authored front side when the affine transform mirrors.
            if(localAxisX.x*localAxisY.y-localAxisX.y*localAxisY.x<0) std::reverse(points.begin(),points.end());
            if(!Physics::BuildChain2D(points,{loop},baked,descriptions,m_error)) return;
        }
        else
        {
            if(points.size()!=2) { m_error="2D Edge requires exactly two vertices."; return; }
            geometry.type=Physics::CollisionGeometryType2D::Segment; geometry.vertices=std::move(points);
            baked.subShapeId=1; descriptions.push_back({std::move(geometry),baked});
        }
        break;
    }
    default: m_error="Invalid 2D collider shape type."; return;
    }
    if(!complex) descriptions.push_back({std::move(geometry),settings});
    const bool sameBody=m_world.lock()==world && m_body==body;
    std::vector<Physics::PhysicsShapeReference> previousShapes;
    if(sameBody) for(const auto handle:m_shapes) if(world->GetShapeBody(handle)==body) previousShapes.emplace_back(handle);
    Physics::PhysicsCommandBuffer commands(world);
    const auto attachment=commands.ReplaceShapes2D(body,std::move(previousShapes),std::move(descriptions)); commands.Commit();
    auto result=commands.GetResult(attachment);
    if(result.status!=Physics::PhysicsCommandStatus::Applied)
    { m_error=shape==EColliderShape::CHAIN?"Invalid 2D chain: static non-trigger geometry required; the previous group is retained.":"Invalid 2D collider geometry; the previous valid shape group is retained."; return; }
    if (!sameBody)
        if (auto previous = m_world.lock())
        { if(m_ownsBody) scene->QueuePhysicsRemoval(previous,m_body); else for(const auto handle:m_shapes) scene->QueuePhysicsRemoval(previous,handle); }
    m_body=body; m_shapes=std::move(result.shapes2D); m_shape=m_shapes.empty()?Physics::ShapeHandle2D{}:m_shapes.front();
    m_world=world; m_ownsBody=rigidbody==nullptr; m_dirty=false; updateStaticPose();
    m_syncedCenter = settings.offset; m_syncedSize = halfSize; m_syncedRotation = settings.rotation;
    m_syncedRadius = scaledRadius; m_syncedHeight = scaledHeight; m_error.clear(); scene->NotifyPhysicsChanged();
    m_syncedAxisX=localAxisX; m_syncedAxisY=localAxisY;
}
void PhysicsCollider2DComponent::SetShape(EColliderShape value)
{ if (value >= EColliderShape::BOX && value <= EColliderShape::CHAIN && shape != value) { shape = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetShapeValue(uint32_t value)
{ if (value <= static_cast<uint32_t>(EColliderShape::CHAIN)) SetShape(static_cast<EColliderShape>(value)); }
void PhysicsCollider2DComponent::SetSize(Maths::Vector2 value) { if (Finite(value) && value.x > 0 && value.y > 0) { size = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetRadius(float value) { if (std::isfinite(value) && value > 0) { radius = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetHalfHeight(float value) { if (std::isfinite(value) && value > 1e-6f) { halfHeight = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetCenter(Maths::Vector2 value) { if (Finite(value)) { center = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetLocalRotation(float value) { if (std::isfinite(value)) { localRotation = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetTrigger(bool value) { if (trigger != value) { trigger = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetLayers(uint32_t value) { if (layers != value) { layers = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetCollisionMask(uint32_t value) { if (collisionMask != value) { collisionMask = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetDensity(float value) { if (std::isfinite(value) && value >= 0) { density = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetFriction(float value) { if (std::isfinite(value) && value >= 0) { friction = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetRestitution(float value) { if (std::isfinite(value) && value >= 0 && value <= 1) { restitution = value; m_dirty = true; } }
void PhysicsCollider2DComponent::SetPhysicsMaterial(Serialize::PPtr<Assets::PhysicsMaterial> value)
{ if(physicsMaterial!=value) { physicsMaterial=value; m_dirty=true; } }
void PhysicsCollider2DComponent::SetVertices(const NLS::Array<Maths::Vector2>& value)
{
    if(value.size()>Physics::MaxCompoundShapes2D+1) { m_error="2D collider vertex count exceeds 257."; return; }
    if(vertices!=value) { vertices=value; m_dirty=true; }
}
void PhysicsCollider2DComponent::SetLoop(bool value) { if(loop!=value) { loop=value; m_dirty=true; } }
bool PhysicsCollider2DComponent::IsQueryReady() const
{
    if(const auto world=m_world.lock()) return !m_shapes.empty() && std::all_of(m_shapes.begin(),m_shapes.end(),[&](auto shape){return world->GetShapeBody(shape)==m_body;});
    return false;
}
}
