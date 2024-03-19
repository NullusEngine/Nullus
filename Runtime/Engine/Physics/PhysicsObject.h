#pragma once
#include "Vector3.h"
#include "Matrix3.h"
#include "EngineDef.h"
#include "Component.h"
using namespace NLS::Maths;

namespace NLS
{
class CollisionVolume;

namespace Engine
{
class Transform;

class NLS_ENGINE_API PhysicsObject: public Component
{
public:
    PhysicsObject();
    ~PhysicsObject();

    Vector3 GetLinearVelocity() const
    {
        return linearVelocity;
    }

    Vector3 GetAngularVelocity() const
    {
        return angularVelocity;
    }

    Vector3 GetTorque() const
    {
        return torque;
    }

    Vector3 GetForce() const
    {
        return force;
    }

    void SetInverseMass(float invMass)
    {
        inverseMass = invMass;
    }

    float GetInverseMass() const
    {
        return inverseMass;
    }

    float GetFriction() const
    {
        return friction;
    }

    void SetFriction(float friction)
    {
        this->friction = friction;
    }

    float GetElasticity() const
    {
        return elasticity;
    }

    void SetElasticity(float elasticity)
    {
        this->elasticity = elasticity;
    }

    void ApplyAngularImpulse(const Vector3& force);
    void ApplyLinearImpulse(const Vector3& force);

    void AddForce(const Vector3& force);

    void AddForceAtPosition(const Vector3& force, const Vector3& position);

    void AddTorque(const Vector3& torque);


    void ClearForces();

    void SetLinearVelocity(const Vector3& v)
    {
        linearVelocity = v;
    }

    void SetAngularVelocity(const Vector3& v)
    {
        angularVelocity = v;
    }

    void InitCubeInertia();
    void InitSphereInertia();

    void UpdateInertiaTensor();

    Matrix3 GetInertiaTensor() const
    {
        return inverseInteriaTensor;
    }

protected:
    float inverseMass;
    float elasticity = 0.8;
    float friction = 0.8;

    // linear stuff
    Vector3 linearVelocity;
    Vector3 force;


    // angular stuff
    Vector3 angularVelocity;
    Vector3 torque;
    Vector3 inverseInertia;
    Matrix3 inverseInteriaTensor;
};
} // namespace Engine
} // namespace NLS
