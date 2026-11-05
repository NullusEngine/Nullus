#pragma once

#include "Assets/PhysicsMaterial.h"

#include "Components/Component.h"
#include <Physics/PhysicsWorld2D.h>
#include <Reflection/Array.h>
#include <string>
#include "Components/PhysicsCollider2DComponent.generated.h"

namespace NLS::Engine::Components
{
CLASS(NLS_ENGINE_API PhysicsCollider2DComponent, Scriptable, ComponentMenu("Physics 2D/Collider")) : public Component
{
public:
    GENERATED_BODY()
    ENUM(EColliderShape) { BOX = 0, CIRCLE = 1, CAPSULE = 2, POLYGON = 3, EDGE = 4, CHAIN = 5 };
    ~PhysicsCollider2DComponent() override;
    void OnDestroy() override;
    void OnDisable() override;
    void Synchronize();
    void Release();
    void ReleaseToScene(SceneSystem::Scene& scene);
    Physics::BodyHandle2D GetBodyHandle() const { return m_body; }
    Physics::ShapeHandle2D GetShapeHandle() const { return m_shape; }
    const std::vector<Physics::ShapeHandle2D>& GetShapeHandles() const { return m_shapes; }
    bool IsQueryReady() const;
    const std::string& LastError() const { return m_error; }
    FUNCTION() EColliderShape GetShape() const { return shape; }
    FUNCTION() void SetShape(EColliderShape value);
    FUNCTION(Scriptable) uint32_t GetShapeValue() const { return static_cast<uint32_t>(shape); }
    FUNCTION(Scriptable) void SetShapeValue(uint32_t value);
    FUNCTION(Scriptable) Maths::Vector2 GetSize() const { return size; }
    FUNCTION(Scriptable) void SetSize(Maths::Vector2 value);
    FUNCTION(Scriptable) float GetRadius() const { return radius; }
    FUNCTION(Scriptable) void SetRadius(float value);
    FUNCTION(Scriptable) float GetHalfHeight() const { return halfHeight; }
    FUNCTION(Scriptable) void SetHalfHeight(float value);
    FUNCTION(Scriptable) Maths::Vector2 GetCenter() const { return center; }
    FUNCTION(Scriptable) void SetCenter(Maths::Vector2 value);
    FUNCTION(Scriptable) float GetLocalRotation() const { return localRotation; }
    FUNCTION(Scriptable) void SetLocalRotation(float degrees);
    FUNCTION(Scriptable) bool IsTrigger() const { return trigger; }
    FUNCTION(Scriptable) void SetTrigger(bool value);
    FUNCTION(Scriptable) uint32_t GetLayers() const { return layers; }
    FUNCTION(Scriptable) void SetLayers(uint32_t value);
    FUNCTION(Scriptable) uint32_t GetCollisionMask() const { return collisionMask; }
    FUNCTION(Scriptable) void SetCollisionMask(uint32_t value);
    FUNCTION(Scriptable) float GetDensity() const { return density; }
    FUNCTION(Scriptable) void SetDensity(float value);
    FUNCTION(Scriptable) float GetFriction() const { return friction; }
    FUNCTION(Scriptable) void SetFriction(float value);
    FUNCTION(Scriptable) float GetRestitution() const { return restitution; }
    FUNCTION(Scriptable) void SetRestitution(float value);
    FUNCTION() Serialize::PPtr<Assets::PhysicsMaterial> GetPhysicsMaterial() const { return physicsMaterial; }
    FUNCTION() void SetPhysicsMaterial(Serialize::PPtr<Assets::PhysicsMaterial> value);
    FUNCTION() NLS::Array<Maths::Vector2> GetVertices() const { return vertices; }
    FUNCTION() void SetVertices(const NLS::Array<Maths::Vector2>& value);
    FUNCTION(Scriptable) bool IsLoop() const { return loop; }
    FUNCTION(Scriptable) void SetLoop(bool value);
private:
    void ReleaseWithContext(SceneSystem::Scene* scene);
    PROPERTY(Getter(GetShape), Setter(SetShape)) EColliderShape shape = EColliderShape::BOX;
    PROPERTY(DisplayName("Size (meters)"), Getter(GetSize), Setter(SetSize)) Maths::Vector2 size{1, 1};
    PROPERTY(Getter(GetRadius), Setter(SetRadius)) float radius = 0.5f;
    PROPERTY(DisplayName("Segment Half Height"), Getter(GetHalfHeight), Setter(SetHalfHeight)) float halfHeight = 0.5f;
    PROPERTY(Getter(GetCenter), Setter(SetCenter)) Maths::Vector2 center;
    PROPERTY(DisplayName("Local Rotation (degrees)"), Getter(GetLocalRotation), Setter(SetLocalRotation)) float localRotation = 0;
    PROPERTY(Getter(IsTrigger), Setter(SetTrigger)) bool trigger = false;
    PROPERTY(Getter(GetLayers), Setter(SetLayers)) uint32_t layers = 1;
    PROPERTY(Getter(GetCollisionMask), Setter(SetCollisionMask)) uint32_t collisionMask = ~0u;
    PROPERTY(Getter(GetDensity), Setter(SetDensity)) float density = 1;
    PROPERTY(Getter(GetFriction), Setter(SetFriction)) float friction = 0.5f;
    PROPERTY(Getter(GetRestitution), Setter(SetRestitution)) float restitution = 0;
    PROPERTY(Getter(GetPhysicsMaterial), Setter(SetPhysicsMaterial)) Serialize::PPtr<Assets::PhysicsMaterial> physicsMaterial;
    PROPERTY(Getter(GetVertices), Setter(SetVertices)) NLS::Array<Maths::Vector2> vertices{{-0.5f,-0.5f},{0.5f,-0.5f},{0.5f,0.5f},{-0.5f,0.5f}};
    PROPERTY(Getter(IsLoop), Setter(SetLoop)) bool loop = false;
    std::shared_ptr<Assets::PhysicsMaterial> m_materialOwner;
    std::shared_ptr<const Assets::PhysicsMaterialData> m_materialData;
    Physics::BodyHandle2D m_body;
    Physics::ShapeHandle2D m_shape;
    std::vector<Physics::ShapeHandle2D> m_shapes;
    std::weak_ptr<Physics::PhysicsWorld2D> m_world;
    bool m_ownsBody = false, m_dirty = true;
    Maths::Vector2 m_syncedCenter, m_syncedSize;
    Maths::Vector2 m_syncedAxisX,m_syncedAxisY;
    float m_syncedRotation = 0, m_syncedRadius = 0, m_syncedHeight = 0;
    std::string m_error;
};
}
