#include "Components/PhysicsColliderComponent.h"
#include "Components/RigidbodyComponent.h"
#include "Physics3DTransform.h"
#include "PhysicsShapeBuilder.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include <Physics/PhysicsCommandBuffer.h>
#include <algorithm>
#include <cmath>

namespace NLS::Engine::Components
{
using namespace Physics3DDetail;
namespace
{
const Maths::Quaternion kCapsuleAxisAlignment{0,0,0.70710678f,0.70710678f};
RigidbodyComponent* FindRigidbody(GameObject& owner)
{
    for (auto* object=&owner; object; object=object->GetParent())
        if (auto* candidate=object->GetComponent<RigidbodyComponent>())
            return candidate->IsActiveAndEnabled() ? candidate : nullptr;
    return nullptr;
}
}
PhysicsColliderComponent::PhysicsColliderComponent() = default;
PhysicsColliderComponent::~PhysicsColliderComponent() { Release(); }
void PhysicsColliderComponent::OnCreate() { if (!gameobject() || !gameobject()->GetScene() || !gameobject()->GetScene()->IsPlaying()) Synchronize(); }
void PhysicsColliderComponent::OnUpdate(float) { if (!gameobject() || !gameobject()->GetScene() || !gameobject()->GetScene()->IsPlaying()) Synchronize(); }
void PhysicsColliderComponent::OnDestroy() { Release(); }
void PhysicsColliderComponent::OnDisable() { Release(); }

void PhysicsColliderComponent::Synchronize()
{
    auto* owner=gameobject(); auto* scene=owner ? owner->GetScene() : nullptr;
    if (!scene || !IsActiveAndEnabled()) { Release(); return; }
    if (scene->IsPhysicsMutationDeferred() || (scene->IsPlaying() && !scene->IsPhysicsSynchronizing())) return;
    Frame frame; if (!Capture(*owner->GetTransform(),frame,m_error)) return;
    std::shared_ptr<const Assets::PhysicsMaterialData> materialData;
    if (!physicsMaterial.IsNull())
    {
        Serialize::ObjectIdentifier reference;
        if (Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(physicsMaterial.GetInstanceID(),reference) && reference.IsAsset())
        {
            auto asset=Assets::LoadPhysicsMaterial(reference,m_error); if (!asset) return;
            m_materialOwner=std::move(asset); materialData=m_materialOwner->Capture();
        }
        else if (const auto* asset=physicsMaterial.Get()) materialData=asset->Capture();
        if (!materialData) { m_error="Physics material reference is unresolved."; return; }
    }
    if (materialData!=m_materialData) { m_materialData=std::move(materialData); m_dirty=true; }
    if (physicsMaterial.IsNull()) m_materialOwner.reset();
    std::shared_ptr<const Physics::CookedCollisionMesh> meshData;
    if (shape==EColliderShape::COLLISION_MESH)
    {
        if (frame.reflected) { m_error="Cooked mesh colliders require reflection baked into the collision asset's bakeScale."; return; }
        Serialize::ObjectIdentifier reference;
        if (Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(collisionMesh.GetInstanceID(),reference) && reference.IsAsset())
        {
            auto asset=Assets::LoadCollisionMeshAsset(reference,m_error); if (!asset) return;
            m_meshOwner=std::move(asset); meshData=m_meshOwner->Capture();
        }
        else if (const auto* asset=collisionMesh.Get()) meshData=asset->Capture();
        if (!meshData) { m_error="Collision mesh reference is unresolved."; return; }
    }
    if (meshData!=m_meshData) { m_meshData=std::move(meshData); m_dirty=true; }
    if (shape!=EColliderShape::COLLISION_MESH) m_meshOwner.reset();
    std::shared_ptr<const Assets::PhysicsShapeData> shapeData;
    if(shape==EColliderShape::SHAPE_ASSET)
    {
        Serialize::ObjectIdentifier reference;
        if(Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(shapeResource.GetInstanceID(),reference) && reference.IsAsset())
        {
            auto asset=Assets::LoadPhysicsShapeAsset(reference,m_error); if(!asset) return;
            m_shapeOwner=std::move(asset); shapeData=m_shapeOwner->Capture();
        }
        else if(const auto* asset=shapeResource.Get()) shapeData=asset->Capture();
        if(!shapeData) { m_error="Shape resource reference is unresolved."; return; }
    }
    if(shapeData!=m_shapeData) { m_shapeData=std::move(shapeData); m_dirty=true; }
    if(shape!=EColliderShape::SHAPE_ASSET) m_shapeOwner.reset();
    auto* rigidbody=FindRigidbody(*owner);
    if (m_meshData && m_meshData->GetKind()!=Physics::CollisionMeshKind::Convex && ((rigidbody && scene->IsPlaying()) || trigger))
    { m_error="Triangle mesh and heightfield colliders require a static body and cannot be triggers."; return; }
    const auto& world=scene->GetPhysicsWorld(); if (!world) { Release(); return; }
    const auto mutation=world->BeginMutationBatch();
    Physics::BodyHandle body;
    Frame root=frame;
    if (rigidbody)
    {
        rigidbody->Synchronize(); body=rigidbody->GetBodyHandle();
        if (!body.IsValid() || !Capture(*rigidbody->gameobject()->GetTransform(),root,m_error) ||
            !Capture(*owner->GetTransform(),frame,m_error))
        { Release(); if (m_error.empty()) m_error=rigidbody->LastError(); return; }
    }
    else
    {
        if (!m_ownsBody || m_world.lock()!=world || !world->IsValid(m_body))
        {
            Release(); Physics::BodySettings settings; settings.position=frame.position; settings.rotation=frame.rotation;
            Physics::PhysicsCommandBuffer commands(world);
            const auto creation=commands.CreateBody(settings); commands.Commit();
            m_body=commands.GetResult(creation).body; m_world=world; m_ownsBody=true;
        }
        body=m_body;
        if (!body.IsValid()) { m_error="Cannot create the static 3D collider body."; return; }
    }
    const auto inverse=Maths::Quaternion::Inverse(root.rotation);
    const auto localXAxis=inverse*frame.xAxis, localYAxis=inverse*frame.yAxis, localZAxis=inverse*frame.zAxis;
    Physics::ShapeSettings settings;
    settings.localPosition=inverse*(frame.position+frame.xAxis*center.x+frame.yAxis*center.y+frame.zAxis*center.z-root.position);
    settings.localRotation=inverse*frame.rotation;
    settings.layer=layers; settings.collisionMask=collisionMask; settings.trigger=trigger;
    settings.staticFriction=friction; settings.dynamicFriction=friction; settings.restitution=restitution;
    if (m_materialData)
    {
        settings.staticFriction=m_materialData->staticFriction; settings.dynamicFriction=m_materialData->dynamicFriction;
        settings.restitution=m_materialData->restitution; settings.frictionCombine=m_materialData->frictionCombine;
        settings.restitutionCombine=m_materialData->restitutionCombine;
    }
    settings.owner=static_cast<uint64_t>(GetInstanceID());
    if (shape==EColliderShape::CAPSULE) settings.localRotation=settings.localRotation*kCapsuleAxisAlignment;
    ShapeBuild built;
    if(m_shapeData)
    {
        if(!BuildShapeResource(*m_shapeData,frame,root,center,settings,rigidbody && scene->IsPlaying(),built,m_error)) return;
    }
    else built.shapes.push_back({InlineGeometry(frame.scale),settings});
    if(built.snapshots!=m_partSnapshots) m_dirty=true;
    const auto updateStaticPose=[&]
    {
        if(!rigidbody)
        {
            Maths::Vector3 position; Maths::Quaternion rotation;
            if(world->GetTransform(body,position,rotation) && (!Same(position,frame.position) || !Same(rotation,frame.rotation)))
            { world->SetTransform(body,frame.position,frame.rotation); scene->NotifyPhysicsChanged(); }
        }
    };
    if (!m_shapes.empty() && m_world.lock()==world && m_body==body && IsQueryReady() && !m_dirty &&
        Same(settings.localPosition,m_syncedPosition) && Same(settings.localRotation,m_syncedRotation) &&
        Same(localXAxis,m_syncedXAxis) && Same(localYAxis,m_syncedYAxis) && Same(localZAxis,m_syncedZAxis))
    { updateStaticPose(); m_error.clear(); m_simulatedBody=rigidbody && scene->IsPlaying(); return; }
    const bool sameBody=m_world.lock()==world && m_body==body;
    std::vector<Physics::PhysicsShapeReference> previousShapes;
    if(sameBody) for(const auto handle:m_shapes) if(world->GetShapeBody(handle)==body) previousShapes.emplace_back(handle);
    Physics::PhysicsCommandBuffer commands(world);
    const auto replacement=commands.ReplaceShapes(body,std::move(previousShapes),std::move(built.shapes)); commands.Commit();
    auto result=commands.GetResult(replacement);
    if(result.status!=Physics::PhysicsCommandStatus::Applied)
    { m_error="Invalid 3D collider geometry; the previous valid shape group is retained."; return; }
    if(!sameBody) if(auto previous=m_world.lock())
    {
        if(m_ownsBody) scene->QueuePhysicsRemoval(previous,m_body);
        else for(const auto handle:m_shapes) scene->QueuePhysicsRemoval(previous,handle);
    }
    m_body=body; m_shapes=std::move(result.shapes); m_shape=m_shapes.empty()?Physics::ShapeHandle{}:m_shapes.front();
    m_partSnapshots=std::move(built.snapshots); m_partResidents=std::move(built.residents);
    m_world=world; m_ownsBody=rigidbody==nullptr; updateStaticPose();
    m_simulatedBody=rigidbody && scene->IsPlaying(); m_dirty=false; m_hasSyncedPose=true;
    m_syncedPosition=settings.localPosition; m_syncedRotation=settings.localRotation; m_syncedScale=frame.scale;
    m_syncedXAxis=localXAxis; m_syncedYAxis=localYAxis; m_syncedZAxis=localZAxis;
    m_error.clear(); scene->NotifyPhysicsChanged();
}
void PhysicsColliderComponent::Release()
{ ReleaseWithContext(nullptr); }
void PhysicsColliderComponent::ReleaseToScene(SceneSystem::Scene& scene)
{ ReleaseWithContext(&scene); }
void PhysicsColliderComponent::ReleaseWithContext(SceneSystem::Scene* context)
{
    if (auto world=m_world.lock())
    {
        auto* scene=context?context:(gameobject()?gameobject()->GetScene():nullptr);
        if (m_ownsBody) { if (scene) scene->QueuePhysicsRemoval(world,m_body); else world->Remove(m_body); }
        else for(const auto shape:m_shapes) { if(scene) scene->QueuePhysicsRemoval(world,shape); else world->Remove(shape); }
    }
    m_body={}; m_shape={}; m_shapes.clear(); m_world.reset(); m_ownsBody=false; m_hasSyncedPose=false; m_simulatedBody=false; m_dirty=true;
    m_partResidents.clear(); m_partSnapshots.clear();
}
void PhysicsColliderComponent::ReadSimulationPose()
{ if (auto* owner=gameobject()) if (auto* body=FindRigidbody(*owner)) body->ReadSimulationPose(); }
Physics::CollisionGeometry3D PhysicsColliderComponent::InlineGeometry(Maths::Vector3 scale) const
{
    Physics::CollisionGeometry3D geometry;
    switch(shape)
    {
    case EColliderShape::SPHERE:
        geometry.type=Physics::CollisionGeometryType3D::Sphere;
        geometry.radius=std::abs(radius)*(std::max)({scale.x,scale.y,scale.z}); break;
    case EColliderShape::CAPSULE:
        geometry.type=Physics::CollisionGeometryType3D::Capsule;
        geometry.radius=std::abs(radius)*(std::max)(scale.x,scale.z); geometry.halfHeight=std::abs(halfHeight)*scale.y; break;
    case EColliderShape::CYLINDER:
        geometry.type=Physics::CollisionGeometryType3D::Cylinder;
        geometry.radius=std::abs(radius)*(std::max)(scale.x,scale.z); geometry.halfHeight=std::abs(halfHeight)*scale.y; break;
    case EColliderShape::BOX:
        geometry.halfExtents={std::abs(size.x)*scale.x,std::abs(size.y)*scale.y,std::abs(size.z)*scale.z}; break;
    case EColliderShape::COLLISION_MESH:
        geometry.type=Physics::CollisionGeometryType3D::CookedMesh; geometry.cookedMesh=m_meshData; geometry.meshScale=scale; break;
    default: geometry.halfExtents={}; break;
    }
    return geometry;
}
void PhysicsColliderComponent::SetCenter(Maths::Vector3 value) { if(Finite(value)) { center=value; MarkDirty(); } }
void PhysicsColliderComponent::SetCollisionMask(uint32_t value) { if(collisionMask!=value) { collisionMask=value; MarkDirty(); } }
void PhysicsColliderComponent::SetFriction(float value)
{ if(std::isfinite(value) && value>=0 && friction!=value) { friction=value; MarkDirty(); } }
void PhysicsColliderComponent::SetRestitution(float value)
{ if(std::isfinite(value) && value>=0 && value<=1 && restitution!=value) { restitution=value; MarkDirty(); } }
void PhysicsColliderComponent::SetPhysicsMaterial(Serialize::PPtr<Assets::PhysicsMaterial> value)
{ if(physicsMaterial!=value) { physicsMaterial=value; MarkDirty(); } }
void PhysicsColliderComponent::SetCollisionMesh(Serialize::PPtr<Assets::CollisionMeshAsset> value)
{ if(collisionMesh!=value) { collisionMesh=value; MarkDirty(); } }
void PhysicsColliderComponent::SetShapeResource(Serialize::PPtr<Assets::PhysicsShapeAsset> value)
{ if(shapeResource!=value) { shapeResource=value; MarkDirty(); } }

PhysicsColliderComponent::EColliderShape PhysicsColliderComponent::GetShape() const
{
    return shape;
}

void PhysicsColliderComponent::SetShape(EColliderShape value)
{
    if (shape == value)
        return;
    shape = value;
    MarkDirty();
}

uint32_t PhysicsColliderComponent::GetShapeValue() const
{
    return static_cast<uint32_t>(shape);
}

void PhysicsColliderComponent::SetShapeValue(uint32_t value)
{
    if (value > static_cast<uint32_t>(EColliderShape::SHAPE_ASSET))
        return;
    SetShape(static_cast<EColliderShape>(value));
}

const Maths::Vector3& PhysicsColliderComponent::GetSize() const
{
    return size;
}

void PhysicsColliderComponent::SetSize(const Maths::Vector3& value)
{
    size = value;
    MarkDirty();
}

float PhysicsColliderComponent::GetRadius() const
{
    return radius;
}

void PhysicsColliderComponent::SetRadius(float value)
{
    if (radius == value)
        return;
    radius = value;
    MarkDirty();
}

float PhysicsColliderComponent::GetHalfHeight() const
{
    return halfHeight;
}

void PhysicsColliderComponent::SetHalfHeight(float value)
{
    if (halfHeight == value)
        return;
    halfHeight = value;
    MarkDirty();
}

uint32_t PhysicsColliderComponent::GetLayers() const
{
    return layers;
}

void PhysicsColliderComponent::SetLayers(uint32_t value)
{
    if (layers == value)
        return;
    layers = value;
    MarkDirty();
}

bool PhysicsColliderComponent::IsTrigger() const
{
    return trigger;
}

void PhysicsColliderComponent::SetTrigger(bool value)
{
    if (trigger == value)
        return;
    trigger = value;
    MarkDirty();
}

bool PhysicsColliderComponent::IsQueryReady() const
{
    if (const auto world=m_world.lock()) return !m_shapes.empty() && std::all_of(m_shapes.begin(),m_shapes.end(),[&](auto shape){return world->GetShapeBody(shape)==m_body;});
    return false;
}

void PhysicsColliderComponent::MarkDirty()
{
    m_dirty = true;
    MarkRenderStateChanged();
}
}
