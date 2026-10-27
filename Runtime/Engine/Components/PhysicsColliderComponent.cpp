#include "Components/PhysicsColliderComponent.h"

#include <algorithm>
#include <cmath>

#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"

namespace NLS::Engine::Components
{
namespace
{
constexpr float kMinimumExtent = 1.0e-3f;
constexpr float kPositionEpsilon = 1.0e-6f;
constexpr float kRotationEpsilon = 1.0e-5f;

// Rotates the X-aligned PhysX capsule geometry onto the engine's Y up axis.
const Maths::Quaternion kCapsuleAxisAlignment{0.0f, 0.0f, 0.70710678f, 0.70710678f};

float FiniteOrFallback(float value, float fallback)
{
    return std::isfinite(value) ? value : fallback;
}

float AbsoluteScale(float value)
{
    return (std::max)(std::abs(FiniteOrFallback(value, 1.0f)), kMinimumExtent);
}

float AbsoluteExtent(float value)
{
    return (std::max)(std::abs(FiniteOrFallback(value, 0.0f)), kMinimumExtent);
}

bool SamePosition(const Maths::Vector3& left, const Maths::Vector3& right)
{
    return std::abs(left.x - right.x) <= kPositionEpsilon &&
        std::abs(left.y - right.y) <= kPositionEpsilon &&
        std::abs(left.z - right.z) <= kPositionEpsilon;
}

bool SameRotation(const Maths::Quaternion& left, const Maths::Quaternion& right)
{
    const auto dot = left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w;
    return std::abs(dot) >= 1.0f - kRotationEpsilon;
}
}

PhysicsColliderComponent::PhysicsColliderComponent() = default;
PhysicsColliderComponent::~PhysicsColliderComponent() = default;

void PhysicsColliderComponent::OnCreate()
{
    Synchronize();
}

void PhysicsColliderComponent::OnUpdate(float)
{
    Synchronize();
}

void PhysicsColliderComponent::OnDestroy()
{
    Release();
}

void PhysicsColliderComponent::Synchronize()
{
    if (!IsActiveAndEnabled())
    {
        Release();
        return;
    }

    auto* owner = gameobject();
    auto* scene = owner != nullptr ? owner->GetScene() : nullptr;
    auto* transform = owner != nullptr ? owner->GetTransform() : nullptr;
    if (scene == nullptr || transform == nullptr)
        return;

    const auto& world = scene->GetPhysicsWorld();
    if (world == nullptr)
    {
        Release();
        return;
    }

    const auto& position = transform->GetWorldPosition();
    const auto& rotation = transform->GetWorldRotation();
    if (!m_body.IsValid() || m_dirty)
    {
        if (m_body.IsValid())
            world->Remove(m_body);
        m_body = PublishBody(*world);
        m_dirty = false;
        m_hasSyncedPose = m_body.IsValid();
        if (m_body.IsValid())
        {
            m_syncedPosition = position;
            m_syncedRotation = rotation;
            scene->NotifyPhysicsChanged();
        }
        return;
    }

    if (m_hasSyncedPose && SamePosition(position, m_syncedPosition) && SameRotation(rotation, m_syncedRotation))
        return;

    if (!world->SetTransform(m_body, position, rotation))
        return;

    m_syncedPosition = position;
    m_syncedRotation = rotation;
    m_hasSyncedPose = true;
    scene->NotifyPhysicsChanged();
}

void PhysicsColliderComponent::Release()
{
    if (m_body.IsValid())
    {
        auto* owner = gameobject();
        auto* scene = owner != nullptr ? owner->GetScene() : nullptr;
        auto* world = scene != nullptr ? scene->PeekPhysicsWorld().get() : nullptr;
        if (world != nullptr && world->Remove(m_body))
            scene->NotifyPhysicsChanged();
        m_body = {};
    }
    m_hasSyncedPose = false;
    m_dirty = true;
}

Physics::BodySettings PhysicsColliderComponent::BuildBodySettings() const
{
    Physics::BodySettings settings;
    if (auto* owner = gameobject())
        if (auto* transform = owner->GetTransform())
        {
            settings.position = transform->GetWorldPosition();
            settings.rotation = transform->GetWorldRotation();
        }
    settings.layer = layers;
    settings.trigger = trigger;
    return settings;
}

Physics::BodyHandle PhysicsColliderComponent::PublishBody(Physics::PhysicsWorld& world) const
{
    auto* owner = gameobject();
    auto* transform = owner != nullptr ? owner->GetTransform() : nullptr;
    if (transform == nullptr)
        return {};

    auto settings = BuildBodySettings();
    const auto& worldScale = transform->GetWorldScale();
    const auto scaleX = AbsoluteScale(worldScale.x);
    const auto scaleY = AbsoluteScale(worldScale.y);
    const auto scaleZ = AbsoluteScale(worldScale.z);

    switch (shape)
    {
    case EColliderShape::SPHERE:
    {
        const auto uniformScale = (std::max)({scaleX, scaleY, scaleZ});
        return world.AddSphere(AbsoluteExtent(radius) * uniformScale, settings);
    }
    case EColliderShape::CAPSULE:
    {
        const auto radialScale = (std::max)(scaleX, scaleZ);
        settings.localRotation = kCapsuleAxisAlignment;
        return world.AddCapsule(AbsoluteExtent(radius) * radialScale, AbsoluteExtent(halfHeight) * scaleY, settings);
    }
    case EColliderShape::BOX:
    default:
        return world.AddBox({AbsoluteExtent(size.x) * scaleX, AbsoluteExtent(size.y) * scaleY,
            AbsoluteExtent(size.z) * scaleZ}, settings);
    }
}

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
    if (value > static_cast<uint32_t>(EColliderShape::CAPSULE))
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
    return m_body.IsValid();
}

void PhysicsColliderComponent::MarkDirty()
{
    m_dirty = true;
    MarkRenderStateChanged();
}
}
