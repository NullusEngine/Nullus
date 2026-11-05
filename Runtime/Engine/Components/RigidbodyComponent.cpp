#include "RigidbodyComponent.h"
#include "Rigidbody2DComponent.h"
#include "Physics3DTransform.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include <Physics/PhysicsCommandBuffer.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace NLS::Engine::Components
{
using namespace Physics3DDetail;
RigidbodyComponent::~RigidbodyComponent() { Release(); }
void RigidbodyComponent::OnStart() { Synchronize(); }
void RigidbodyComponent::OnDisable() { ResetSimulation(); }
void RigidbodyComponent::OnDestroy() { ResetSimulation(); }
void RigidbodyComponent::ResetSimulation() { Release(); m_commands.clear(); m_simulated=false; }
void RigidbodyComponent::RefreshCollider() { m_dirty = true; }
void RigidbodyComponent::CacheLocalPose()
{
    if (auto* transform = gameobject() ? gameobject()->GetTransform() : nullptr)
    { m_lastLocalPosition=transform->GetLocalPosition(); m_lastLocalRotation=transform->GetLocalRotation(); m_hasPose=true; }
}
void RigidbodyComponent::Release()
{ ReleaseWithContext(nullptr); }
void RigidbodyComponent::ReleaseToScene(SceneSystem::Scene& scene)
{ ReleaseWithContext(&scene); }
void RigidbodyComponent::ReleaseWithContext(SceneSystem::Scene* context)
{
    if (auto world=m_world.lock(); world && m_body.IsValid())
    {
        auto* scene=context?context:(gameobject()?gameobject()->GetScene():nullptr);
        if (scene) scene->QueuePhysicsRemoval(world,m_body);
        else world->Remove(m_body);
    }
    m_body={}; m_world.reset(); m_hasPose=false; m_dirty=true; m_poseHistory.Clear();
}
void RigidbodyComponent::Synchronize()
{
    auto* owner=gameobject(); auto* scene=owner ? owner->GetScene() : nullptr;
    if (!IsActiveAndEnabled() || !scene) { Release(); return; }
    if (scene->IsPhysicsMutationDeferred() || (scene->IsPlaying() && !scene->IsPhysicsSynchronizing())) return;
    if (owner->GetComponent<RigidbodyComponent>()!=this ||
        (owner->GetComponent<Rigidbody2DComponent>() && owner->GetComponent<Rigidbody2DComponent>()->IsActiveAndEnabled()))
    { Release(); m_error="Only one enabled Rigidbody may control an object's Transform, across both dimensions."; return; }
    Frame frame;
    if (!Capture(*owner->GetTransform(),frame,m_error)) { Release(); return; }
    if (scene->IsPlaying() && !kinematic)
    {
        for (auto* object=owner; object; object=object->GetParent())
        {
            const auto scale=object->GetTransform()->GetLocalScale();
            if (scale.x<=0 || scale.y<=0 || scale.z<=0 ||
                (object!=owner && (std::abs(scale.x-scale.y)>1e-5f || std::abs(scale.x-scale.z)>1e-5f)))
            { Release(); m_error="Dynamic 3D bodies require positive scale and uniformly scaled ancestors for world-pose writeback."; return; }
        }
    }
    const auto& world=scene->GetPhysicsWorld(); if (!world) { Release(); return; }
    const bool simulated=scene->IsPlaying();
    const bool authored=m_hasPose && (!Same(owner->GetTransform()->GetLocalPosition(),m_lastLocalPosition) ||
        !Same(owner->GetTransform()->GetLocalRotation(),m_lastLocalRotation));
    Physics::BodyState old;
    const bool live=m_world.lock()==world && world->GetState(m_body,old);
    if (!live || m_dirty || simulated!=m_simulated)
    {
        Physics::BodySettings settings;
        settings.dynamic=simulated; settings.kinematic=kinematic; settings.useGravity=useGravity; settings.mass=mass;
        settings.position=live && m_simulated && !authored ? old.position : frame.position;
        settings.rotation=live && m_simulated && !authored ? old.rotation : frame.rotation;
        settings.velocity=live && m_simulated ? old.velocity : velocity;
        settings.angularVelocity=live && m_simulated ? old.angularVelocity : angularVelocity;
        settings.linearDamping=linearDamping; settings.angularDamping=angularDamping;
        settings.continuousCollision=continuousCollision;
        settings.lockPositionX=(constraints&1)!=0; settings.lockPositionY=(constraints&2)!=0; settings.lockPositionZ=(constraints&4)!=0;
        settings.lockRotationX=(constraints&8)!=0; settings.lockRotationY=(constraints&16)!=0; settings.lockRotationZ=(constraints&32)!=0;
        settings.autoMassProperties=autoMassProperties; settings.centerOfMass=centerOfMass;
        settings.inertiaTensor=inertiaTensor; settings.massFrameRotation=massFrameRotation;
        settings.startAwake=startAwake; settings.sleepThreshold=sleepThreshold;
        if (live && simulated==m_simulated && world->UpdateDynamics(m_body,settings))
        { m_dirty=false; scene->NotifyPhysicsChanged(); }
        else
        {
            Physics::PhysicsCommandBuffer commands(world);
            const auto creation=commands.CreateBody(settings); commands.Commit();
            const auto replacement=commands.GetResult(creation).body;
            if (!replacement.IsValid()) { m_error="Cannot create the 3D rigidbody."; return; }
            Release(); m_body=replacement; m_world=world; m_dirty=false; m_simulated=simulated;
            ResetInterpolation(); CacheLocalPose(); scene->NotifyPhysicsChanged(); return;
        }
    }
    if (!simulated || kinematic || authored)
        if (!Same(old.position,frame.position) || !Same(old.rotation,frame.rotation))
        {
            if (simulated && kinematic) world->SetKinematicTarget(m_body,frame.position,frame.rotation);
            else { world->SetTransform(m_body,frame.position,frame.rotation); ResetInterpolation(); }
            scene->NotifyPhysicsChanged();
        }
    if (simulated && !kinematic && !authored && (!Same(old.position,frame.position) || !Same(old.rotation,frame.rotation)))
    {
        auto* transform=owner->GetTransform(); transform->SetWorldPosition(old.position); transform->SetWorldRotation(old.rotation);
    }
    CacheLocalPose();
}
void RigidbodyComponent::ReadSimulationPose()
{
    if (!m_simulated || !m_body.IsValid()) return;
    auto world=m_world.lock(); Physics::BodyState state;
    if (!world || !world->GetState(m_body,state)) return;
    m_poseHistory.Publish({state.position,state.rotation},state.stepIndex,state.poseRevision);
    if (kinematic) return;
    auto* transform=gameobject()->GetTransform();
    transform->SetWorldPosition(state.position); transform->SetWorldRotation(state.rotation); CacheLocalPose();
}
void RigidbodyComponent::ResetInterpolation()
{
    auto world=m_world.lock(); Physics::BodyState state;
    if (!m_simulated || !world || !world->GetState(m_body,state)) { m_poseHistory.Clear(); return; }
    m_poseHistory.Reset({state.position,state.rotation},state.stepIndex,state.poseRevision);
}
void RigidbodyComponent::RefreshInterpolation()
{
    auto world=m_world.lock(); Physics::BodyState state;
    if (!m_simulated || !world || !world->GetState(m_body,state)) { m_poseHistory.Clear(); return; }
    if (!m_poseHistory.Tracks(state.stepIndex,state.poseRevision))
    {
        m_poseHistory.Reset({state.position,state.rotation},state.stepIndex,state.poseRevision);
        if (auto* scene=gameobject()->GetScene()) scene->MarkRenderContentChanged();
    }
}
bool RigidbodyComponent::TryGetPresentationPose(double alpha, Physics::PhysicsPose& authoring, Physics::PhysicsPose& presented) const
{
    if (!m_simulated || !IsActiveAndEnabled() || !gameobject()) return false;
    auto* transform=gameobject()->GetTransform();
    if (!m_hasPose || !Same(transform->GetLocalPosition(),m_lastLocalPosition) || !Same(transform->GetLocalRotation(),m_lastLocalRotation)) return false;
    Frame frame; std::string error; if (!Capture(*transform,frame,error)) return false;
    auto world=m_world.lock(); Physics::BodyState state;
    if (!world || !world->GetState(m_body,state)) return false;
    authoring={frame.position,frame.rotation};
    if (!m_poseHistory.Tracks(state.stepIndex,state.poseRevision) || !m_poseHistory.Interpolate(alpha,presented))
        presented={state.position,state.rotation};
    return true;
}
void RigidbodyComponent::Queue(Command command)
{
    auto* scene=gameobject() ? gameobject()->GetScene() : nullptr;
    if (scene && scene->IsPlaying() && IsActiveAndEnabled()) m_commands.push_back(command);
}
void RigidbodyComponent::ApplyPendingCommands()
{
    if (auto* scene=gameobject() ? gameobject()->GetScene() : nullptr; scene && scene->IsPhysicsMutationDeferred()) return;
    auto world=m_world.lock();
    if (!world || !m_body.IsValid() || !m_simulated) { m_commands.clear(); return; }
    for (const auto& command:m_commands)
        switch (command.kind)
        {
        case CommandKind::Force: world->ApplyForce(m_body,command.value,command.mode); break;
        case CommandKind::Torque: world->AddTorque(m_body,command.value,command.mode); break;
        case CommandKind::Velocity: world->SetVelocity(m_body,command.value); break;
        case CommandKind::AngularVelocity: world->SetAngularVelocity(m_body,command.value); break;
        case CommandKind::ForceAtPosition: world->AddForceAtPosition(m_body,command.value,command.position,command.mode); break;
        case CommandKind::Awake: world->SetAwake(m_body,command.awake); break;
        case CommandKind::Teleport:
            if (world->SetTransform(m_body,command.value,command.rotation))
            {
                if (!command.keepVelocity) { world->SetVelocity(m_body,{}); world->SetAngularVelocity(m_body,{}); }
                auto* transform=gameobject()->GetTransform();
                transform->SetWorldPosition(command.value); transform->SetWorldRotation(command.rotation); ResetInterpolation(); CacheLocalPose();
            }
            break;
        }
    m_commands.clear();
}
void RigidbodyComponent::SetMass(float value)
{ if (std::isfinite(value)) { mass=std::clamp(value,0.0001f,1000000.0f); m_dirty=true; } }
void RigidbodyComponent::SetKinematic(bool value) { if (kinematic!=value) { kinematic=value; m_dirty=true; } }
void RigidbodyComponent::SetUseGravity(bool value) { if (useGravity!=value) { useGravity=value; m_dirty=true; } }
void RigidbodyComponent::SetLinearDamping(float value)
{ if (std::isfinite(value) && value>=0 && linearDamping!=value) { linearDamping=value; m_dirty=true; } }
void RigidbodyComponent::SetAngularDamping(float value)
{ if (std::isfinite(value) && value>=0 && angularDamping!=value) { angularDamping=value; m_dirty=true; } }
void RigidbodyComponent::SetContinuousCollision(bool value)
{ if (continuousCollision!=value) { continuousCollision=value; m_dirty=true; } }
void RigidbodyComponent::SetConstraints(uint32_t value)
{ if (value<=63 && constraints!=value) { constraints=value; m_dirty=true; } }
void RigidbodyComponent::SetAutoMassProperties(bool value)
{ if (autoMassProperties!=value) { autoMassProperties=value; m_dirty=true; } }
void RigidbodyComponent::SetCenterOfMass(Maths::Vector3 value)
{ if (Finite(value) && centerOfMass!=value) { centerOfMass=value; m_dirty=true; } }
void RigidbodyComponent::SetInertiaTensor(Maths::Vector3 value)
{ if (Finite(value) && value.x>0 && value.y>0 && value.z>0 && std::isfinite(1.0f/value.x) && std::isfinite(1.0f/value.y) &&
    std::isfinite(1.0f/value.z) && inertiaTensor!=value) { inertiaTensor=value; m_dirty=true; } }
void RigidbodyComponent::SetMassFrameRotation(Maths::Quaternion value)
{
    const float length=value.x*value.x+value.y*value.y+value.z*value.z+value.w*value.w;
    if (!std::isfinite(length) || length<=1e-12f) return;
    value=Maths::Quaternion::Normalize(value);
    if (massFrameRotation.x!=value.x || massFrameRotation.y!=value.y || massFrameRotation.z!=value.z || massFrameRotation.w!=value.w)
    { massFrameRotation=value; m_dirty=true; }
}
void RigidbodyComponent::SetStartAwake(bool value)
{ if (startAwake!=value) { startAwake=value; m_dirty=true; } }
void RigidbodyComponent::SetSleepThreshold(float value)
{ if (std::isfinite(value) && value>=0 && value<std::numeric_limits<float>::max() && sleepThreshold!=value) { sleepThreshold=value; m_dirty=true; } }
bool RigidbodyComponent::IsSleeping() const
{
    Physics::BodyState state; auto world=m_world.lock();
    return m_simulated && !kinematic && world && world->GetState(m_body,state) && !state.awake;
}
void RigidbodyComponent::WakeUp() { Queue({CommandKind::Awake}); }
void RigidbodyComponent::Sleep() { Command command{CommandKind::Awake}; command.awake=false; Queue(command); }
Maths::Vector3 RigidbodyComponent::GetVelocity() const
{
    for (auto it=m_commands.rbegin();it!=m_commands.rend();++it) if(it->kind==CommandKind::Velocity) return it->value;
    if (auto world=m_world.lock(); world && m_simulated) return world->GetVelocity(m_body);
    return velocity;
}
Maths::Vector3 RigidbodyComponent::GetAngularVelocity() const
{
    for (auto it=m_commands.rbegin();it!=m_commands.rend();++it) if(it->kind==CommandKind::AngularVelocity) return it->value;
    if (auto world=m_world.lock()) { Physics::BodyState state; if(m_simulated && world->GetState(m_body,state)) return state.angularVelocity; }
    return angularVelocity;
}
void RigidbodyComponent::SetVelocity(Maths::Vector3 value)
{ if (Finite(value)) { velocity=value; Queue({CommandKind::Velocity,value}); } }
void RigidbodyComponent::SetAngularVelocity(Maths::Vector3 value)
{ if (Finite(value)) { angularVelocity=value; Queue({CommandKind::AngularVelocity,value}); } }
void RigidbodyComponent::ApplyForce(Maths::Vector3 value, Physics::ForceMode mode)
{ if (Finite(value) && mode<=Physics::ForceMode::VelocityChange) Queue({CommandKind::Force,value,{},mode}); }
void RigidbodyComponent::AddForce(Maths::Vector3 value) { ApplyForce(value,Physics::ForceMode::Force); }
void RigidbodyComponent::AddImpulse(Maths::Vector3 value) { ApplyForce(value,Physics::ForceMode::Impulse); }
void RigidbodyComponent::AddTorque(Maths::Vector3 value) { ApplyTorque(value,Physics::ForceMode::Force); }
void RigidbodyComponent::ApplyTorque(Maths::Vector3 value, Physics::ForceMode mode)
{ if (Finite(value) && mode<=Physics::ForceMode::VelocityChange) Queue({CommandKind::Torque,value,{},mode}); }
void RigidbodyComponent::AddForceAtPosition(Maths::Vector3 value, Maths::Vector3 position, Physics::ForceMode mode)
{
    if (!Finite(value) || !Finite(position) || mode>Physics::ForceMode::VelocityChange) return;
    Command command{CommandKind::ForceAtPosition,value,{},mode}; command.position=position; Queue(command);
}
void RigidbodyComponent::AddForceWithMode(Maths::Vector3 value, uint32_t mode)
{ if (mode<=static_cast<uint32_t>(Physics::ForceMode::VelocityChange)) ApplyForce(value,static_cast<Physics::ForceMode>(mode)); }
void RigidbodyComponent::AddTorqueWithMode(Maths::Vector3 value, uint32_t mode)
{ if (mode<=static_cast<uint32_t>(Physics::ForceMode::VelocityChange)) ApplyTorque(value,static_cast<Physics::ForceMode>(mode)); }
void RigidbodyComponent::AddForceAtPositionWithMode(Maths::Vector3 value, Maths::Vector3 position, uint32_t mode)
{ if (mode<=static_cast<uint32_t>(Physics::ForceMode::VelocityChange)) AddForceAtPosition(value,position,static_cast<Physics::ForceMode>(mode)); }
void RigidbodyComponent::Teleport(Maths::Vector3 position, Maths::Quaternion rotation, bool keepVelocity)
{
    const double length=double(rotation.x)*rotation.x+double(rotation.y)*rotation.y+double(rotation.z)*rotation.z+double(rotation.w)*rotation.w;
    if (Finite(position) && std::isfinite(length) && length>1e-12) Queue({CommandKind::Teleport,position,rotation,Physics::ForceMode::Force,keepVelocity});
}
}
