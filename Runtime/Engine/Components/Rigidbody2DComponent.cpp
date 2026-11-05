#include "Rigidbody2DComponent.h"
#include "Physics2DTransform.h"
#include "RigidbodyComponent.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include <Physics/PhysicsCommandBuffer.h>
#include <algorithm>
#include <cmath>

namespace NLS::Engine::Components
{
using namespace Physics2DDetail;
Rigidbody2DComponent::~Rigidbody2DComponent() { Release(); }
void Rigidbody2DComponent::OnDestroy() { ResetSimulation(); }
void Rigidbody2DComponent::OnDisable() { ResetSimulation(); }
void Rigidbody2DComponent::ResetSimulation() { Release(); m_commands.clear(); m_simulated=false; }
void Rigidbody2DComponent::CacheLocalPose()
{
    if (auto* transform = gameobject() ? gameobject()->GetTransform() : nullptr)
    { m_lastLocalPosition = transform->GetLocalPosition(); m_lastLocalRotation = transform->GetLocalRotation(); m_hasPose = true; }
}
void Rigidbody2DComponent::Release()
{ ReleaseWithContext(nullptr); }
void Rigidbody2DComponent::ReleaseToScene(SceneSystem::Scene& scene)
{ ReleaseWithContext(&scene); }
void Rigidbody2DComponent::ReleaseWithContext(SceneSystem::Scene* context)
{
    if (auto world = m_world.lock(); world && m_body.IsValid())
    {
        auto* scene=context?context:(gameobject()?gameobject()->GetScene():nullptr);
        if (scene) scene->QueuePhysicsRemoval(world,m_body);
        else world->Remove(m_body);
    }
    m_body = {}; m_world.reset(); m_hasPose = false; m_dirty = true; m_poseHistory.Clear();
}
void Rigidbody2DComponent::Synchronize()
{
    auto* owner = gameobject(); auto* scene = owner ? owner->GetScene() : nullptr;
    if (!IsActiveAndEnabled() || !scene) { Release(); return; }
    if (scene->IsPhysicsMutationDeferred() || (scene->IsPlaying() && !scene->IsPhysicsSynchronizing())) return;
    if (owner->GetComponent<Rigidbody2DComponent>() != this ||
        (owner->GetComponent<RigidbodyComponent>() && owner->GetComponent<RigidbodyComponent>()->IsActiveAndEnabled()))
    { Release(); m_error = "Only one enabled Rigidbody may control an object's Transform, across both dimensions."; return; }
    Frame frame;
    if (!Capture(*owner->GetTransform(), frame, m_error)) { Release(); return; }
    if (scene->IsPlaying() && motionType == EMotionType::DYNAMIC)
        for (auto* object = owner; object; object = object->GetParent())
        {
            const auto scale = object->GetTransform()->GetLocalScale();
            if (scale.x <= 0 || scale.y <= 0 || scale.z <= 0 || (object != owner && std::abs(scale.x - scale.y) > 1e-5f))
            { Release(); m_error = "Dynamic 2D bodies require positive scale and uniformly scaled XY ancestors for world-pose writeback."; return; }
        }
    const auto& world = scene->GetPhysicsWorld2D(); if (!world) { Release(); return; }
    const bool simulated = scene->IsPlaying();
    const bool authored = m_hasPose && (!Same(owner->GetTransform()->GetLocalPosition(), m_lastLocalPosition) ||
        !Same(owner->GetTransform()->GetLocalRotation(), m_lastLocalRotation));
    Physics::BodyState2D old;
    const bool live = m_world.lock() == world && world->GetState(m_body, old);
    if (!live || m_dirty || simulated != m_simulated)
    {
        Physics::BodySettings2D settings;
        settings.type = simulated ? static_cast<Physics::BodyType2D>(motionType) : Physics::BodyType2D::Static;
        settings.position = live && m_simulated && !authored ? old.position : frame.position;
        settings.rotation = live && m_simulated && !authored ? old.rotation : frame.angle;
        settings.velocity = live && m_simulated ? old.velocity : velocity;
        settings.angularVelocity = live && m_simulated ? old.angularVelocity : angularVelocity;
        settings.mass = mass; settings.autoMass = autoMass; settings.gravityScale = gravityScale;
        settings.linearDamping = linearDamping; settings.angularDamping = angularDamping;
        settings.lockRotation = freezeRotation; settings.bullet = bullet;
        if (live && simulated == m_simulated && world->UpdateDynamics(m_body, settings))
        { m_dirty = false; scene->NotifyPhysicsChanged(); }
        else
        {
            Physics::PhysicsCommandBuffer commands(world);
            const auto creation=commands.CreateBody(settings); commands.Commit();
            const auto replacement=commands.GetResult(creation).body2D;
            if (!replacement.IsValid()) { m_error = "Cannot create the 2D rigidbody."; return; }
            Release(); m_world = world; m_body = replacement; m_dirty = false; m_simulated = simulated;
            ResetInterpolation(); CacheLocalPose(); scene->NotifyPhysicsChanged(); return;
        }
    }
    if (!simulated || motionType != EMotionType::DYNAMIC || authored)
    {
        if (!Same(old.position, frame.position) || !SameAngle(old.rotation, frame.angle))
        { world->SetTransform(m_body, frame.position, frame.angle); ResetInterpolation(); scene->NotifyPhysicsChanged(); }
    }
    else if (!Same(old.position,frame.position) || !SameAngle(old.rotation,frame.angle))
    {
        auto* transform=owner->GetTransform(); const float z=transform->GetWorldPosition().z;
        transform->SetWorldPosition({old.position.x,old.position.y,z});
        transform->SetWorldRotation({0,0,std::sin(old.rotation*0.5f),std::cos(old.rotation*0.5f)});
    }
    CacheLocalPose();
}
void Rigidbody2DComponent::ReadSimulationPose()
{
    if (!m_simulated || motionType == EMotionType::STATIC || !m_body.IsValid()) return;
    auto world = m_world.lock(); Physics::BodyState2D state;
    if (!world || !world->GetState(m_body, state)) return;
    auto* transform = gameobject()->GetTransform();
    const auto z = transform->GetWorldPosition().z;
    m_poseHistory.Publish({{state.position.x,state.position.y,z}, {0,0,std::sin(state.rotation*0.5f),std::cos(state.rotation*0.5f)}},state.stepIndex,state.poseRevision);
    transform->SetWorldPosition({state.position.x, state.position.y, z});
    transform->SetWorldRotation({0, 0, std::sin(state.rotation * 0.5f), std::cos(state.rotation * 0.5f)});
    CacheLocalPose();
}
void Rigidbody2DComponent::ResetInterpolation()
{
    auto world=m_world.lock(); Physics::BodyState2D state;
    if (!m_simulated || motionType==EMotionType::STATIC || !world || !world->GetState(m_body,state)) { m_poseHistory.Clear(); return; }
    const float z=gameobject()->GetTransform()->GetWorldPosition().z;
    m_poseHistory.Reset({{state.position.x,state.position.y,z},{0,0,std::sin(state.rotation*0.5f),std::cos(state.rotation*0.5f)}},state.stepIndex,state.poseRevision);
}
void Rigidbody2DComponent::RefreshInterpolation()
{
    auto world=m_world.lock(); Physics::BodyState2D state;
    if (!m_simulated || motionType==EMotionType::STATIC || !world || !world->GetState(m_body,state)) { m_poseHistory.Clear(); return; }
    if (!m_poseHistory.Tracks(state.stepIndex,state.poseRevision))
    {
        ResetInterpolation();
        if (auto* scene=gameobject()->GetScene()) scene->MarkRenderContentChanged();
    }
}
bool Rigidbody2DComponent::TryGetPresentationPose(double alpha, Physics::PhysicsPose& authoring, Physics::PhysicsPose& presented) const
{
    if (!m_simulated || motionType==EMotionType::STATIC || !IsActiveAndEnabled() || !gameobject()) return false;
    auto* transform=gameobject()->GetTransform();
    if (!m_hasPose || !Same(transform->GetLocalPosition(),m_lastLocalPosition) || !Same(transform->GetLocalRotation(),m_lastLocalRotation)) return false;
    Frame frame; std::string error; if (!Capture(*transform,frame,error)) return false;
    auto world=m_world.lock(); Physics::BodyState2D state; if (!world || !world->GetState(m_body,state)) return false;
    const float z=transform->GetWorldPosition().z;
    authoring={{frame.position.x,frame.position.y,z},{0,0,std::sin(frame.angle*0.5f),std::cos(frame.angle*0.5f)}};
    if (!m_poseHistory.Tracks(state.stepIndex,state.poseRevision) || !m_poseHistory.Interpolate(alpha,presented))
        presented={{state.position.x,state.position.y,z},{0,0,std::sin(state.rotation*0.5f),std::cos(state.rotation*0.5f)}};
    presented.position.z=z;
    return true;
}
void Rigidbody2DComponent::Queue(Command command)
{
    auto* scene = gameobject() ? gameobject()->GetScene() : nullptr;
    if (scene && scene->IsPlaying() && IsActiveAndEnabled()) m_commands.push_back(command);
}
void Rigidbody2DComponent::ApplyPendingCommands()
{
    if (auto* scene=gameobject() ? gameobject()->GetScene() : nullptr; scene && scene->IsPhysicsMutationDeferred()) return;
    auto world = m_world.lock();
    if (!world || !m_body.IsValid() || !m_simulated) { m_commands.clear(); return; }
    for (const auto& command : m_commands)
    {
        switch (command.kind)
        {
        case CommandKind::Force: world->AddForce(m_body, command.value, command.mode); break;
        case CommandKind::Torque: world->AddTorque(m_body, command.scalar, command.mode); break;
        case CommandKind::Velocity:
        { Physics::BodyState2D state; if (world->GetState(m_body, state)) world->SetVelocity(m_body, command.value, state.angularVelocity); break; }
        case CommandKind::AngularVelocity:
        { Physics::BodyState2D state; if (world->GetState(m_body, state)) world->SetVelocity(m_body, state.velocity, command.scalar); break; }
        case CommandKind::Teleport:
            world->SetTransform(m_body, command.value, command.scalar, command.keepVelocity); ResetInterpolation(); ReadSimulationPose(); break;
        }
    }
    m_commands.clear();
}
void Rigidbody2DComponent::SetMotionType(EMotionType value)
{ if (value >= EMotionType::STATIC && value <= EMotionType::KINEMATIC && motionType != value) { motionType = value; m_dirty = true; } }
void Rigidbody2DComponent::SetMotionTypeValue(uint32_t value)
{ if (value <= static_cast<uint32_t>(EMotionType::KINEMATIC)) SetMotionType(static_cast<EMotionType>(value)); }
float Rigidbody2DComponent::GetMass() const
{ if (auto world = m_world.lock()) { Physics::BodyState2D state; if (m_simulated && world->GetState(m_body, state)) return state.mass; } return mass; }
void Rigidbody2DComponent::SetMass(float value) { if (std::isfinite(value) && value > 0) { mass = std::clamp(value, 0.0001f, 1000000.0f); m_dirty = true; } }
void Rigidbody2DComponent::SetAutoMass(bool value) { if (autoMass != value) { autoMass = value; m_dirty = true; } }
void Rigidbody2DComponent::SetGravityScale(float value) { if (std::isfinite(value) && gravityScale != value) { gravityScale = value; m_dirty = true; } }
void Rigidbody2DComponent::SetLinearDamping(float value) { if (std::isfinite(value) && value >= 0 && linearDamping != value) { linearDamping = value; m_dirty = true; } }
void Rigidbody2DComponent::SetAngularDamping(float value) { if (std::isfinite(value) && value >= 0 && angularDamping != value) { angularDamping = value; m_dirty = true; } }
void Rigidbody2DComponent::SetFreezeRotation(bool value) { if (freezeRotation != value) { freezeRotation = value; m_dirty = true; } }
void Rigidbody2DComponent::SetBullet(bool value) { if (bullet != value) { bullet = value; m_dirty = true; } }
Maths::Vector2 Rigidbody2DComponent::GetVelocity() const
{
    for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it) if (it->kind == CommandKind::Velocity) return it->value;
    if (auto world = m_world.lock()) { Physics::BodyState2D state; if (m_simulated && world->GetState(m_body, state)) return state.velocity; }
    return velocity;
}
float Rigidbody2DComponent::GetAngularVelocity() const
{
    for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it) if (it->kind == CommandKind::AngularVelocity) return it->scalar;
    if (auto world = m_world.lock()) { Physics::BodyState2D state; if (m_simulated && world->GetState(m_body, state)) return state.angularVelocity; }
    return angularVelocity;
}
void Rigidbody2DComponent::SetVelocity(Maths::Vector2 value) { if (Finite(value)) { velocity = value; Queue({CommandKind::Velocity, value}); } }
void Rigidbody2DComponent::SetAngularVelocity(float value) { if (std::isfinite(value)) { angularVelocity = value; Queue({CommandKind::AngularVelocity, {}, value}); } }
void Rigidbody2DComponent::ApplyForce(Maths::Vector2 value, Physics::ForceMode2D mode)
{ if (Finite(value) && mode <= Physics::ForceMode2D::VelocityChange) Queue({CommandKind::Force, value, 0, mode}); }
void Rigidbody2DComponent::AddForce(Maths::Vector2 value) { ApplyForce(value, Physics::ForceMode2D::Force); }
void Rigidbody2DComponent::AddImpulse(Maths::Vector2 value) { ApplyForce(value, Physics::ForceMode2D::Impulse); }
void Rigidbody2DComponent::AddTorque(float value) { if (std::isfinite(value)) Queue({CommandKind::Torque, {}, value}); }
void Rigidbody2DComponent::Teleport(Maths::Vector2 position, float rotationRadians, bool keepVelocity)
{ if (Finite(position) && std::isfinite(rotationRadians)) Queue({CommandKind::Teleport, position, rotationRadians, Physics::ForceMode2D::Force, keepVelocity}); }
}
