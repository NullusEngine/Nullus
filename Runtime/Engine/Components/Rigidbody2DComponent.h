#pragma once

#include "Components/Component.h"
#include <Physics/PhysicsWorld2D.h>
#include <Math/Quaternion.h>
#include <Math/Vector3.h>
#include <string>
#include <Physics/PhysicsPoseHistory.h>
#include "Components/Rigidbody2DComponent.generated.h"
#include <memory>

namespace NLS::Engine::Components
{
CLASS(NLS_ENGINE_API Rigidbody2DComponent, Scriptable, ComponentMenu("Physics 2D/Rigidbody"), DisallowMultipleComponent) : public Component
{
public:
    GENERATED_BODY()
    ENUM(EMotionType) { STATIC = 0, DYNAMIC = 1, KINEMATIC = 2 };
    ~Rigidbody2DComponent() override;
    void OnDestroy() override;
    void OnDisable() override;
    void Synchronize();
    void ReadSimulationPose();
    void ApplyPendingCommands();
    void Release();
    void ReleaseToScene(SceneSystem::Scene& scene);
    void ResetSimulation();
    void ResetInterpolation();
    void RefreshInterpolation();
    bool TryGetPresentationPose(double alpha, Physics::PhysicsPose& authoring, Physics::PhysicsPose& presented) const;
    Physics::BodyHandle2D GetBodyHandle() const { return m_body; }
    const std::string& LastError() const { return m_error; }

    FUNCTION() EMotionType GetMotionType() const { return motionType; }
    FUNCTION() void SetMotionType(EMotionType value);
    FUNCTION(Scriptable) uint32_t GetMotionTypeValue() const { return static_cast<uint32_t>(motionType); }
    FUNCTION(Scriptable) void SetMotionTypeValue(uint32_t value);
    FUNCTION(Scriptable) float GetMass() const;
    FUNCTION(Scriptable) void SetMass(float value);
    FUNCTION(Scriptable) bool GetAutoMass() const { return autoMass; }
    FUNCTION(Scriptable) void SetAutoMass(bool value);
    FUNCTION(Scriptable) float GetGravityScale() const { return gravityScale; }
    FUNCTION(Scriptable) void SetGravityScale(float value);
    FUNCTION(Scriptable) float GetLinearDamping() const { return linearDamping; }
    FUNCTION(Scriptable) void SetLinearDamping(float value);
    FUNCTION(Scriptable) float GetAngularDamping() const { return angularDamping; }
    FUNCTION(Scriptable) void SetAngularDamping(float value);
    FUNCTION(Scriptable) bool GetFreezeRotation() const { return freezeRotation; }
    FUNCTION(Scriptable) void SetFreezeRotation(bool value);
    FUNCTION(Scriptable) bool GetBullet() const { return bullet; }
    FUNCTION(Scriptable) void SetBullet(bool value);
    FUNCTION(Scriptable) Maths::Vector2 GetVelocity() const;
    FUNCTION(Scriptable) void SetVelocity(Maths::Vector2 value);
    FUNCTION(Scriptable) float GetAngularVelocity() const;
    FUNCTION(Scriptable) void SetAngularVelocity(float value);
    FUNCTION(Scriptable) void AddForce(Maths::Vector2 value);
    FUNCTION(Scriptable) void AddImpulse(Maths::Vector2 value);
    FUNCTION(Scriptable) void AddTorque(float value);
    FUNCTION(Scriptable) void Teleport(Maths::Vector2 position, float rotationRadians, bool keepVelocity = true);
    void ApplyForce(Maths::Vector2 value, Physics::ForceMode2D mode);

private:
    void ReleaseWithContext(SceneSystem::Scene* scene);
    enum class CommandKind { Force, Torque, Velocity, AngularVelocity, Teleport };
    struct Command
    {
        CommandKind kind;
        Maths::Vector2 value;
        float scalar = 0;
        Physics::ForceMode2D mode = Physics::ForceMode2D::Force;
        bool keepVelocity = true;
    };
    void Queue(Command command);
    void CacheLocalPose();
    PROPERTY(Getter(GetMotionType), Setter(SetMotionType)) EMotionType motionType = EMotionType::DYNAMIC;
    PROPERTY(Getter(GetAutoMass), Setter(SetAutoMass)) bool autoMass = true;
    PROPERTY(Getter(GetMass), Setter(SetMass), Range(0.0001f, 1000000.0f)) float mass = 1;
    PROPERTY(Getter(GetGravityScale), Setter(SetGravityScale)) float gravityScale = 1;
    PROPERTY(Getter(GetLinearDamping), Setter(SetLinearDamping), Range(0.0f, 1000.0f)) float linearDamping = 0;
    PROPERTY(Getter(GetAngularDamping), Setter(SetAngularDamping), Range(0.0f, 1000.0f)) float angularDamping = 0;
    PROPERTY(Getter(GetFreezeRotation), Setter(SetFreezeRotation)) bool freezeRotation = false;
    PROPERTY(Getter(GetBullet), Setter(SetBullet)) bool bullet = false;
    PROPERTY(Getter(GetVelocity), Setter(SetVelocity)) Maths::Vector2 velocity;
    PROPERTY(Getter(GetAngularVelocity), Setter(SetAngularVelocity)) float angularVelocity = 0;
    Physics::BodyHandle2D m_body;
    std::weak_ptr<Physics::PhysicsWorld2D> m_world;
    std::vector<Command> m_commands;
    Maths::Vector3 m_lastLocalPosition;
    Maths::Quaternion m_lastLocalRotation;
    bool m_dirty = true, m_simulated = false, m_hasPose = false;
    std::string m_error;
    Physics::PhysicsPoseHistory m_poseHistory;
};
}
