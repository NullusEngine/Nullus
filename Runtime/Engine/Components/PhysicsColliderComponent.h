#pragma once

#include <cstdint>

#include "Components/Component.h"
#include "EngineDef.h"
#include "Math/Quaternion.h"
#include "Math/Vector3.h"
#include "Physics/PhysicsWorld.h"
#include "Reflection/Macros.h"
#include "Components/PhysicsColliderComponent.generated.h"

namespace NLS::Engine::Components
{
/**
 * Static scene query geometry consumed by particle collision and other physics
 * queries. Bodies are published through the owning scene's update
 * synchronization point, so read views never observe a half-applied collider.
 */
CLASS(NLS_ENGINE_API PhysicsColliderComponent, Scriptable, ComponentMenu("Physics/Collider"), DisallowMultipleComponent) : public Component
{
public:
    GENERATED_BODY()

    /** Query shape family published into the scene's PhysX world. */
    ENUM(EColliderShape)
    {
        BOX = 0,
        SPHERE = 1,
        CAPSULE = 2
    };

    PhysicsColliderComponent();
    ~PhysicsColliderComponent() override;

    void OnCreate() override;
    void OnUpdate(float deltaTime) override;
    void OnDestroy() override;

    // Publishes or refreshes this collider inside the owning scene's query
    // world. The scene calls this before component updates, so particles sample
    // the current frame's geometry.
    void Synchronize();
    void Release();

    // Inspector-facing typed accessors; the script manifest only carries the
    // portable value pair below, matching UICanvasComponent's enum binding.
    FUNCTION()
    EColliderShape GetShape() const;
    FUNCTION()
    void SetShape(EColliderShape shape);
    FUNCTION(Scriptable)
    uint32_t GetShapeValue() const;
    FUNCTION(Scriptable)
    void SetShapeValue(uint32_t shape);
    FUNCTION(Scriptable)
    const Maths::Vector3& GetSize() const;
    FUNCTION(Scriptable)
    void SetSize(const Maths::Vector3& size);
    FUNCTION(Scriptable)
    float GetRadius() const;
    FUNCTION(Scriptable)
    void SetRadius(float radius);
    FUNCTION(Scriptable)
    float GetHalfHeight() const;
    FUNCTION(Scriptable)
    void SetHalfHeight(float halfHeight);
    FUNCTION(Scriptable)
    uint32_t GetLayers() const;
    FUNCTION(Scriptable)
    void SetLayers(uint32_t layers);
    FUNCTION(Scriptable)
    bool IsTrigger() const;
    FUNCTION(Scriptable)
    void SetTrigger(bool trigger);
    [[nodiscard]] bool IsQueryReady() const;

private:
    void MarkDirty();
    [[nodiscard]] Physics::BodySettings BuildBodySettings() const;
    [[nodiscard]] Physics::BodyHandle PublishBody(Physics::PhysicsWorld& world) const;

    PROPERTY(DisplayName("Shape"), Getter(GetShape), Setter(SetShape))
    EColliderShape shape = EColliderShape::BOX;
    PROPERTY(DisplayName("Half Extents"), Getter(GetSize), Setter(SetSize))
    Maths::Vector3 size{0.5f, 0.5f, 0.5f};
    PROPERTY(DisplayName("Radius"), Getter(GetRadius), Setter(SetRadius))
    float radius = 0.5f;
    PROPERTY(DisplayName("Half Height"), Getter(GetHalfHeight), Setter(SetHalfHeight))
    float halfHeight = 0.5f;
    PROPERTY(DisplayName("Layers"), Getter(GetLayers), Setter(SetLayers))
    uint32_t layers = 1u;
    PROPERTY(DisplayName("Trigger"), Getter(IsTrigger), Setter(SetTrigger))
    bool trigger = false;

    Physics::BodyHandle m_body;
    Maths::Vector3 m_syncedPosition;
    Maths::Quaternion m_syncedRotation = Maths::Quaternion::Identity;
    bool m_dirty = true;
    bool m_hasSyncedPose = false;
};
}
