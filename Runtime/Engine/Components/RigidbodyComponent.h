#pragma once
#include "Components/Component.h"
#include "Math/Vector3.h"
#include "Physics/PhysicsWorld.h"
#include <string>
#include <Physics/PhysicsPoseHistory.h>
#include "Components/RigidbodyComponent.generated.h"

namespace NLS::Engine::Components
{
CLASS(NLS_ENGINE_API RigidbodyComponent, Scriptable, ComponentMenu("Physics/Rigidbody"), DisallowMultipleComponent) : public Component
{
public:
    GENERATED_BODY()
    ~RigidbodyComponent() override;
    void OnStart() override;
    void OnDisable() override;
    void OnDestroy() override;
    void Synchronize();
    void ReadSimulationPose();
    void ApplyPendingCommands();
    void Release();
    // Scene bridge cleanup remains valid after generic owner detachment.
    void ReleaseToScene(SceneSystem::Scene& scene);
    void ResetSimulation();
    void ResetInterpolation();
    void RefreshInterpolation();
    bool TryGetPresentationPose(double alpha, Physics::PhysicsPose& authoring, Physics::PhysicsPose& presented) const;
    Physics::BodyHandle GetBodyHandle() const { return m_body; }
    bool HasSimulatedBody() const { return m_simulated && m_body.IsValid(); }
    const std::string& LastError() const { return m_error; }
    FUNCTION(Scriptable) float GetMass() const { return mass; }
    FUNCTION(Scriptable) void SetMass(float value);
    FUNCTION(Scriptable) bool IsKinematic() const { return kinematic; }
    FUNCTION(Scriptable) void SetKinematic(bool value);
    FUNCTION(Scriptable) bool GetUseGravity() const { return useGravity; }
    FUNCTION(Scriptable) void SetUseGravity(bool value);
    FUNCTION(Scriptable) Maths::Vector3 GetVelocity() const;
    FUNCTION(Scriptable) void SetVelocity(Maths::Vector3 value);
    FUNCTION(Scriptable) void AddForce(Maths::Vector3 force);
    FUNCTION(Scriptable) void AddImpulse(Maths::Vector3 impulse);
    FUNCTION(Scriptable) void AddTorque(Maths::Vector3 torque);
    FUNCTION(Scriptable) Maths::Vector3 GetAngularVelocity() const;
    FUNCTION(Scriptable) void SetAngularVelocity(Maths::Vector3 value);
    FUNCTION(Scriptable) float GetLinearDamping() const { return linearDamping; }
    FUNCTION(Scriptable) void SetLinearDamping(float value);
    FUNCTION(Scriptable) float GetAngularDamping() const { return angularDamping; }
    FUNCTION(Scriptable) void SetAngularDamping(float value);
    FUNCTION(Scriptable) bool GetContinuousCollision() const { return continuousCollision; }
    FUNCTION(Scriptable) void SetContinuousCollision(bool value);
    FUNCTION(Scriptable) void Teleport(Maths::Vector3 position, Maths::Quaternion rotation, bool keepVelocity = true);
    FUNCTION(Scriptable) uint32_t GetConstraints() const { return constraints; }
    FUNCTION(Scriptable) void SetConstraints(uint32_t value);
    FUNCTION(Scriptable) bool GetAutoMassProperties() const { return autoMassProperties; }
    FUNCTION(Scriptable) void SetAutoMassProperties(bool value);
    FUNCTION(Scriptable) Maths::Vector3 GetCenterOfMass() const { return centerOfMass; }
    FUNCTION(Scriptable) void SetCenterOfMass(Maths::Vector3 value);
    FUNCTION(Scriptable) Maths::Vector3 GetInertiaTensor() const { return inertiaTensor; }
    FUNCTION(Scriptable) void SetInertiaTensor(Maths::Vector3 value);
    FUNCTION(Scriptable) Maths::Quaternion GetMassFrameRotation() const { return massFrameRotation; }
    FUNCTION(Scriptable) void SetMassFrameRotation(Maths::Quaternion value);
    FUNCTION(Scriptable) bool GetStartAwake() const { return startAwake; }
    FUNCTION(Scriptable) void SetStartAwake(bool value);
    FUNCTION(Scriptable) float GetSleepThreshold() const { return sleepThreshold; }
    FUNCTION(Scriptable) void SetSleepThreshold(float value);
    FUNCTION(Scriptable) bool IsSleeping() const;
    FUNCTION(Scriptable) void WakeUp();
    FUNCTION(Scriptable) void Sleep();
    FUNCTION(Scriptable) void AddForceWithMode(Maths::Vector3 force, uint32_t mode);
    FUNCTION(Scriptable) void AddTorqueWithMode(Maths::Vector3 torque, uint32_t mode);
    FUNCTION(Scriptable) void AddForceAtPositionWithMode(Maths::Vector3 force, Maths::Vector3 position, uint32_t mode);
    void ApplyForce(Maths::Vector3 force, Physics::ForceMode mode);
    void ApplyTorque(Maths::Vector3 torque, Physics::ForceMode mode);
    void AddForceAtPosition(Maths::Vector3 force, Maths::Vector3 position, Physics::ForceMode mode = Physics::ForceMode::Force);
    void RefreshCollider();
private:
    void ReleaseWithContext(SceneSystem::Scene* scene);
    void CacheLocalPose();
    enum class CommandKind { Force, Torque, Velocity, AngularVelocity, Teleport, ForceAtPosition, Awake };
    struct Command
    {
        CommandKind kind;
        Maths::Vector3 value;
        Maths::Quaternion rotation;
        Physics::ForceMode mode = Physics::ForceMode::Force;
        bool keepVelocity = true;
        Maths::Vector3 position;
        bool awake = true;
    };
    void Queue(Command command);
    PROPERTY(Getter(GetMass), Setter(SetMass), Range(0.0001f, 1000000.0f)) float mass = 1;
    PROPERTY(Getter(IsKinematic), Setter(SetKinematic)) bool kinematic = false;
    PROPERTY(Getter(GetUseGravity), Setter(SetUseGravity)) bool useGravity = true;
    PROPERTY(Getter(GetVelocity), Setter(SetVelocity)) Maths::Vector3 velocity;
    PROPERTY(Getter(GetAngularVelocity), Setter(SetAngularVelocity)) Maths::Vector3 angularVelocity;
    PROPERTY(Getter(GetLinearDamping), Setter(SetLinearDamping)) float linearDamping = 0;
    PROPERTY(Getter(GetAngularDamping), Setter(SetAngularDamping)) float angularDamping = 0.05f;
    PROPERTY(Getter(GetContinuousCollision), Setter(SetContinuousCollision)) bool continuousCollision = false;
    PROPERTY(Getter(GetConstraints), Setter(SetConstraints), Range(0, 63)) uint32_t constraints = 0;
    PROPERTY(Getter(GetAutoMassProperties), Setter(SetAutoMassProperties)) bool autoMassProperties = true;
    PROPERTY(Getter(GetCenterOfMass), Setter(SetCenterOfMass)) Maths::Vector3 centerOfMass;
    PROPERTY(Getter(GetInertiaTensor), Setter(SetInertiaTensor)) Maths::Vector3 inertiaTensor{1,1,1};
    PROPERTY(Getter(GetMassFrameRotation), Setter(SetMassFrameRotation)) Maths::Quaternion massFrameRotation = Maths::Quaternion::Identity;
    PROPERTY(Getter(GetStartAwake), Setter(SetStartAwake)) bool startAwake = true;
    PROPERTY(Getter(GetSleepThreshold), Setter(SetSleepThreshold)) float sleepThreshold = 0.005f;
    Physics::BodyHandle m_body;
    std::weak_ptr<Physics::PhysicsWorld> m_world;
    std::vector<Command> m_commands;
    Maths::Vector3 m_lastLocalPosition;
    Maths::Quaternion m_lastLocalRotation;
    bool m_hasPose = false, m_dirty = true, m_simulated = false;
    std::string m_error;
    Physics::PhysicsPoseHistory m_poseHistory;
};

enum class RigidbodyConstraints : uint32_t
{
    None=0, FreezePositionX=1, FreezePositionY=2, FreezePositionZ=4,
    FreezeRotationX=8, FreezeRotationY=16, FreezeRotationZ=32, FreezePosition=7, FreezeRotation=56, FreezeAll=63
};
}
