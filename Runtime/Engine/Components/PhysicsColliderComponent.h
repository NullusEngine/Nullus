#pragma once

#include <cstdint>
#include <string>

#include "Components/Component.h"
#include "EngineDef.h"
#include "Math/Quaternion.h"
#include "Math/Vector3.h"
#include "Physics/PhysicsWorld.h"
#include "Reflection/Macros.h"
#include "Assets/PhysicsMaterial.h"
#include "Assets/CollisionMeshAsset.h"
#include "Assets/PhysicsShapeAsset.h"
#include "Components/PhysicsColliderComponent.generated.h"
#include <memory>

namespace NLS::Engine::Components
{
/**
 * A shape attached to the nearest 3D rigidbody, or an owned static query body.
 * The scene publishes changes in one dimension's mutation batch.
 */
CLASS(NLS_ENGINE_API PhysicsColliderComponent, Scriptable, ComponentMenu("Physics/Collider")) : public Component
{
public:
    GENERATED_BODY()

    /** Query shape family published into the scene's PhysX world. */
    ENUM(EColliderShape)
    {
        BOX = 0,
        SPHERE = 1,
        CAPSULE = 2,
        CYLINDER = 3,
        COLLISION_MESH = 4,
        SHAPE_ASSET = 5
    };

    PhysicsColliderComponent();
    ~PhysicsColliderComponent() override;

    void OnCreate() override;
    void OnUpdate(float deltaTime) override;
    void OnDestroy() override;
    void OnDisable() override;

    // Publishes or refreshes this collider inside the owning scene's query
    // world. The scene calls this before component updates, so particles sample
    // the current frame's geometry.
    void Synchronize();
    void Release();
    void ReleaseToScene(SceneSystem::Scene& scene);

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
    FUNCTION(Scriptable) Maths::Vector3 GetCenter() const { return center; }
    FUNCTION(Scriptable) void SetCenter(Maths::Vector3 value);
    FUNCTION(Scriptable) uint32_t GetCollisionMask() const { return collisionMask; }
    FUNCTION(Scriptable) void SetCollisionMask(uint32_t value);
    FUNCTION(Scriptable) float GetFriction() const { return friction; }
    FUNCTION(Scriptable) void SetFriction(float value);
    FUNCTION(Scriptable) float GetRestitution() const { return restitution; }
    FUNCTION(Scriptable) void SetRestitution(float value);
    FUNCTION() Serialize::PPtr<Assets::PhysicsMaterial> GetPhysicsMaterial() const { return physicsMaterial; }
    FUNCTION() void SetPhysicsMaterial(Serialize::PPtr<Assets::PhysicsMaterial> value);
    FUNCTION() Serialize::PPtr<Assets::CollisionMeshAsset> GetCollisionMesh() const { return collisionMesh; }
    FUNCTION() void SetCollisionMesh(Serialize::PPtr<Assets::CollisionMeshAsset> value);
    FUNCTION() Serialize::PPtr<Assets::PhysicsShapeAsset> GetShapeResource() const { return shapeResource; }
    FUNCTION() void SetShapeResource(Serialize::PPtr<Assets::PhysicsShapeAsset> value);
    [[nodiscard]] bool IsQueryReady() const;
    [[nodiscard]] Physics::BodyHandle GetBodyHandle() const { return m_body; }
    [[nodiscard]] Physics::ShapeHandle GetShapeHandle() const { return m_shape; }
    [[nodiscard]] const std::vector<Physics::ShapeHandle>& GetShapeHandles() const { return m_shapes; }
    const std::string& LastError() const { return m_error; }
    void ReadSimulationPose();
    bool HasSimulatedBody() const { return m_simulatedBody; }

private:
    void ReleaseWithContext(SceneSystem::Scene* scene);
    void MarkDirty();
    [[nodiscard]] Physics::CollisionGeometry3D InlineGeometry(Maths::Vector3 scale) const;

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
    PROPERTY(Getter(GetCenter), Setter(SetCenter)) Maths::Vector3 center;
    PROPERTY(Getter(GetCollisionMask), Setter(SetCollisionMask)) uint32_t collisionMask = ~0u;
    PROPERTY(Getter(GetFriction), Setter(SetFriction)) float friction = 0.5f;
    PROPERTY(Getter(GetRestitution), Setter(SetRestitution)) float restitution = 0.5f;
    PROPERTY(Getter(GetPhysicsMaterial), Setter(SetPhysicsMaterial)) Serialize::PPtr<Assets::PhysicsMaterial> physicsMaterial;
    PROPERTY(Getter(GetCollisionMesh), Setter(SetCollisionMesh)) Serialize::PPtr<Assets::CollisionMeshAsset> collisionMesh;
    PROPERTY(Getter(GetShapeResource), Setter(SetShapeResource)) Serialize::PPtr<Assets::PhysicsShapeAsset> shapeResource;

    std::shared_ptr<Assets::PhysicsMaterial> m_materialOwner;
    std::shared_ptr<Assets::CollisionMeshAsset> m_meshOwner;
    std::shared_ptr<const Assets::PhysicsMaterialData> m_materialData;
    std::shared_ptr<const Physics::CookedCollisionMesh> m_meshData;
    std::shared_ptr<Assets::PhysicsShapeAsset> m_shapeOwner;
    std::shared_ptr<const Assets::PhysicsShapeData> m_shapeData;
    std::vector<std::shared_ptr<Object>> m_partResidents;
    std::vector<std::shared_ptr<const void>> m_partSnapshots;

    Physics::BodyHandle m_body;
    Physics::ShapeHandle m_shape;
    std::vector<Physics::ShapeHandle> m_shapes;
    std::weak_ptr<Physics::PhysicsWorld> m_world;
    Maths::Vector3 m_syncedPosition;
    Maths::Quaternion m_syncedRotation = Maths::Quaternion::Identity;
    Maths::Vector3 m_syncedScale;
    Maths::Vector3 m_syncedXAxis,m_syncedYAxis,m_syncedZAxis;
    bool m_dirty = true;
    bool m_hasSyncedPose = false;
    bool m_simulatedBody = false;
    bool m_ownsBody = false;
    std::string m_error;
};
}
