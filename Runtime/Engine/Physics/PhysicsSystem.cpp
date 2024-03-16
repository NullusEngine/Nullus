#include "PhysicsSystem.h"
#include "PhysicsObject.h"
#include "Engine/GameObject.h"
#include "Engine/CollosionDetection/CollisionDetection.h"
#include "Math/Quaternion.h"

#include "Constraint.h"

#include "Debug.h"

#include <functional>
using namespace NLS;
using namespace Engine;

/*

These two variables help define the relationship between positions
and the forces that are added to objects to change those positions

*/

PhysicsSystem::PhysicsSystem(GameWorld& g)
    : gameWorld(g)
{
    applyGravity = false;
    useBroadPhase = true;
    dTOffset = 0.0f;
    globalDamping = 0.995f;
    linearDamping = 0.4f;
    SetGravity(Vector3(0.0f, -9.8f, 0.0f));
}

PhysicsSystem::~PhysicsSystem()
{
}

void PhysicsSystem::SetGravity(const Vector3& g)
{
    gravity = g;
}

/*

If the 'game' is ever reset, the PhysicsSystem must be
'cleared' to remove any old collisions that might still
be hanging around in the collision list. If your engine
is expanded to allow objects to be removed from the world,
you'll need to iterate through this collisions list to remove
any collisions they are in.

*/
void PhysicsSystem::Clear()
{
    allCollisions.clear();
}

/*

This is the core of the physics engine update

*/
int constraintIterationCount = 10;

// This is the fixed timestep we'd LIKE to have
const int idealHZ = 120;
const float idealDT = 1.0f / idealHZ;

/*
This is the fixed update we actually have...
If physics takes too long it starts to kill the framerate, it'll drop the
iteration count down until the FPS stabilises, even if that ends up
being at a low rate.
*/
int realHZ = idealHZ;
float realDT = idealDT;

void PhysicsSystem::Update(float dt)
{
    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::B))
    {
        useBroadPhase = !useBroadPhase;
        std::cout << "Setting broadphase to " << useBroadPhase << std::endl;
    }
    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::I))
    {
        constraintIterationCount--;
        std::cout << "Setting constraint iterations to " << constraintIterationCount << std::endl;
    }
    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::O))
    {
        constraintIterationCount++;
        std::cout << "Setting constraint iterations to " << constraintIterationCount << std::endl;
    }

    dTOffset += dt; // We accumulate time delta here - there might be remainders from previous frame!

    GameTimer t;
    t.GetTimeDeltaSeconds();

    if (useBroadPhase)
    {
        UpdateObjectAABBs();
    }

    while (dTOffset >= realDT)
    {
        IntegrateAccel(realDT); // Update accelerations from external forces
        if (useBroadPhase)
        {
            BroadPhase();
            NarrowPhase();
        }
        else
        {
            BasicCollisionDetection();
        }

        // This is our simple iterative solver -
        // we just run things multiple times, slowly moving things forward
        // and then rechecking that the constraints have been met
        float constraintDt = realDT / (float)constraintIterationCount;
        for (int i = 0; i < constraintIterationCount; ++i)
        {
            UpdateConstraints(constraintDt);
        }
        IntegrateVelocity(realDT); // update positions from new velocity changes

        dTOffset -= realDT;
    }

    ClearForces(); // Once we've finished with the forces, reset them to zero

    UpdateCollisionList(); // Remove any old collisions

    t.Tick();
    float updateTime = t.GetTimeDeltaSeconds();

    // Uh oh, physics is taking too long...
    if (updateTime > realDT)
    {
        realHZ /= 2;
        realDT *= 2;
        std::cout << "Dropping iteration count due to long physics time...(now " << realHZ << ")\n";
    }
    else if (dt * 2 < realDT)
    { // we have plenty of room to increase iteration count!
        int temp = realHZ;
        realHZ *= 2;
        realDT /= 2;

        if (realHZ > idealHZ)
        {
            realHZ = idealHZ;
            realDT = idealDT;
        }
        if (temp != realHZ)
        {
            std::cout << "Raising iteration count due to short physics time...(now " << realHZ << ")\n";
        }
    }
}

/*
Later on we're going to need to keep track of collisions
across multiple frames, so we store them in a set.

The first time they are added, we tell the objects they are colliding.
The frame they are to be removed, we tell them they're no longer colliding.

From this simple mechanism, we we build up gameplay interactions inside the
OnCollisionBegin / OnCollisionEnd functions (removing health when hit by a
rocket launcher, gaining a point when the player hits the gold coin, and so on).
*/
void PhysicsSystem::UpdateCollisionList()
{
    for (std::set<CollisionDetection::CollisionInfo>::iterator i = allCollisions.begin(); i != allCollisions.end();)
    {
        if ((*i).framesLeft == numCollisionFrames)
        {
            i->a->OnCollisionBegin(i->b);
            i->b->OnCollisionBegin(i->a);
        }
        (*i).framesLeft = (*i).framesLeft - 1;
        if ((*i).framesLeft < 0)
        {
            i->a->OnCollisionEnd(i->b);
            i->b->OnCollisionEnd(i->a);
            i = allCollisions.erase(i);
        }
        else
        {
            ++i;
        }
    }
}

void PhysicsSystem::UpdateObjectAABBs()
{
    /*gameWorld.OperateOnContents(
        [](GameObject* g) {
            g->UpdateBroadphaseAABB();
        }
    );*/
    std::vector<GameObject*>::const_iterator first;
    std::vector<GameObject*>::const_iterator last;
    gameWorld.GetObjectIterators(first, last);
    for (auto i = first; i != last; ++i)
    {
        (*i)->UpdateBroadphaseAABB();
    }
}

/*

This is how we'll be doing collision detection in tutorial 4.
We step thorugh every pair of objects once (the inner for loop offset
ensures this), and determine whether they collide, and if so, add them
to the collision set for later processing. The set will guarantee that
a particular pair will only be added once, so objects colliding for
multiple frames won't flood the set with duplicates.
*/
void PhysicsSystem::BasicCollisionDetection()
{
    std::vector<GameObject*>::const_iterator first;
    std::vector<GameObject*>::const_iterator last;
    gameWorld.GetObjectIterators(first, last);

    for (auto i = first; i != last; ++i)
    {
        if ((*i)->GetPhysicsObject() == nullptr)
        {
            continue;
        }
        for (auto j = i + 1; j != last; ++j)
        {
            if ((*j)->GetPhysicsObject() == nullptr)
            {
                continue;
            }
            CollisionDetection::CollisionInfo info;
            if (CollisionDetection::ObjectIntersection(*i, *j, info))
            {
                // std::cout << "Collision between " << (*i) -> GetName() << " and " << (*j) -> GetName() << std::endl;
                ImpulseResolveCollision(*info.a, *info.b, info.point);
                info.framesLeft = numCollisionFrames;
                allCollisions.insert(info);
            }
        }
    }
}

void NLS::Engine::PhysicsSystem::ResolveSpringCollision(GameObject& a, GameObject& b, CollisionDetection::ContactPoint& p) const
{
    PhysicsObject* physA = a.GetPhysicsObject();
    PhysicsObject* physB = b.GetPhysicsObject();

    Transform& transformA = a.GetTransform();
    Transform& transformB = b.GetTransform();

    float totalMass = physA->GetInverseMass() + physB->GetInverseMass();

    if (totalMass == 0)
    {
        return; // two static objects ??
    }
    // Separate them out using projection
    transformA.SetPosition(transformA.GetPosition() - (p.normal * p.penetration * (physA->GetInverseMass() / totalMass)));
    transformB.SetPosition(transformB.GetPosition() + (p.normal * p.penetration * (physB->GetInverseMass() / totalMass)));
}

/*

In tutorial 5, we start determining the correct response to a collision,
so that objects separate back out.

*/
void PhysicsSystem::ImpulseResolveCollision(GameObject& a, GameObject& b, CollisionDetection::ContactPoint& p) const
{
    PhysicsObject* physA = a.GetPhysicsObject();
    PhysicsObject* physB = b.GetPhysicsObject();

    Transform& transformA = a.GetTransform();
    Transform& transformB = b.GetTransform();

    float totalMass = physA->GetInverseMass() + physB->GetInverseMass();

    if (totalMass == 0)
    {
        return; // two static objects ??
    }

    // Separate them out using projection
    transformA.SetPosition(transformA.GetPosition() - (p.normal * p.penetration * (physA->GetInverseMass() / totalMass)));
    transformB.SetPosition(transformB.GetPosition() + (p.normal * p.penetration * (physB->GetInverseMass() / totalMass)));

    Vector3 relativeA = p.localA;
    Vector3 relativeB = p.localB;

    Vector3 angVelocityA = Vector3::Cross(physA->GetAngularVelocity(), relativeA);
    Vector3 angVelocityB = Vector3::Cross(physB->GetAngularVelocity(), relativeB);

    Vector3 fullVelocityA = physA->GetLinearVelocity() + angVelocityA;
    Vector3 fullVelocityB = physB->GetLinearVelocity() + angVelocityB;

    Vector3 contactVelocity = fullVelocityB - fullVelocityA;

    float impulseForce = Vector3::Dot(contactVelocity, p.normal);
    // now to work out the effect of inertia ....
    Vector3 inertiaA = Vector3::Cross(physA->GetInertiaTensor() * Vector3::Cross(relativeA, p.normal), relativeA);
    Vector3 inertiaB = Vector3::Cross(physB->GetInertiaTensor() * Vector3::Cross(relativeB, p.normal), relativeB);
    float angularEffect = Vector3::Dot(inertiaA + inertiaB, p.normal);

    // float cRestitution = 0.66f; // disperse some kinectic energy
    float cRestitution = physA->GetElasticity() * physB->GetElasticity();
    float j = (-(1.0f + cRestitution) * impulseForce) / (totalMass + angularEffect);

    Vector3 fullImpulse = p.normal * j;

    physA->ApplyLinearImpulse(-fullImpulse);
    physB->ApplyLinearImpulse(fullImpulse);

    physA->ApplyAngularImpulse(Vector3::Cross(relativeA, -fullImpulse));
    physB->ApplyAngularImpulse(Vector3::Cross(relativeB, fullImpulse));
}

/*

Later, we replace the BasicCollisionDetection method with a broadphase
and a narrowphase collision detection method. In the broad phase, we
split the world up using an acceleration structure, so that we can only
compare the collisions that we absolutely need to.

*/

void PhysicsSystem::BroadPhase()
{
    broadphaseCollisions.clear();
    QuadTree<GameObject*> tree(Vector2(1024, 1024), 7, 6);
    std::vector<GameObject*>::const_iterator first;
    std::vector<GameObject*>::const_iterator last;
    gameWorld.GetObjectIterators(first, last);
    for (auto i = first; i != last; ++i)
    {
        Vector3 halfSizes;
        if (!(*i)->GetBroadphaseAABB(halfSizes))
        {
            continue;
        }
        Vector3 pos = (*i)->GetTransform().GetPosition();
        tree.Insert(*i, pos, halfSizes);
    }
    tree.OperateOnContents([&](std::list<QuadTreeEntry<GameObject*>>& data)
                           {
		CollisionDetection::CollisionInfo info;
		for (auto i = data.begin(); i != data.end(); ++i) {
			for (auto j = std::next(i); j != data.end(); ++j) {
				// is this pair of items already in the collision set -
				// if the same pair is in another quadtree node together etc
				info.a = min((*i).object, (*j).object);
				info.b = max((*i).object, (*j).object);
				broadphaseCollisions.insert(info);
			}
		} });
}

/*

The broadphase will now only give us likely collisions, so we can now go through them,
and work out if they are truly colliding, and if so, add them into the main collision list
*/
void PhysicsSystem::NarrowPhase()
{
    for (std::set<CollisionDetection::CollisionInfo>::iterator i = broadphaseCollisions.begin(); i != broadphaseCollisions.end(); ++i)
    {
        CollisionDetection::CollisionInfo info = *i;
        if (CollisionDetection::ObjectIntersection(info.a, info.b, info))
        {
            info.framesLeft = numCollisionFrames;
            ImpulseResolveCollision(*info.a, *info.b, info.point);
            allCollisions.insert(info); // insert into our main set
        }
    }
}

/*
Integration of acceleration and velocity is split up, so that we can
move objects multiple times during the course of a PhysicsUpdate,
without worrying about repeated forces accumulating etc.

This function will update both linear and angular acceleration,
based on any forces that have been accumulated in the objects during
the course of the previous game frame.
*/
void PhysicsSystem::IntegrateAccel(float dt)
{
    std::vector<GameObject*>::const_iterator first;
    std::vector<GameObject*>::const_iterator last;
    gameWorld.GetObjectIterators(first, last);

    for (auto i = first; i != last; ++i)
    {
        PhysicsObject* object = (*i)->GetPhysicsObject();
        if (object == nullptr)
        {
            continue; // No physics object for this GameObject !
        }
        float inverseMass = object->GetInverseMass();

        Vector3 linearVel = object->GetLinearVelocity();
        Vector3 force = object->GetForce();
        Vector3 accel = force * inverseMass;

        if (applyGravity && inverseMass > 0)
        {
            accel += gravity; // don ’t move infinitely heavy things
        }
        linearVel += accel * dt; // integrate accel !
        object->SetLinearVelocity(linearVel);

        // Angular stuff
        Vector3 torque = object->GetTorque();
        Vector3 angVel = object->GetAngularVelocity();
        object->UpdateInertiaTensor(); // update tensor vs orientation
        Vector3 angAccel = object->GetInertiaTensor() * torque;
        angVel += angAccel * dt; // integrate angular accel !
        object->SetAngularVelocity(angVel);
    }
}
/*
This function integrates linear and angular velocity into
position and orientation. It may be called multiple times
throughout a physics update, to slowly move the objects through
the world, looking for collisions.
*/
void PhysicsSystem::IntegrateVelocity(float dt)
{
    std::vector<GameObject*>::const_iterator first;
    std::vector<GameObject*>::const_iterator last;
    gameWorld.GetObjectIterators(first, last);
    float frameLinearDamping = 1.0f - (linearDamping * dt);

    for (auto i = first; i != last; ++i)
    {
        PhysicsObject* object = (*i)->GetPhysicsObject();
        if (object == nullptr)
        {
            continue;
        }
        Transform& transform = (*i)->GetTransform();
        // Position Stuff
        Vector3 position = transform.GetPosition();
        Vector3 linearVel = object->GetLinearVelocity();
        position += linearVel * dt;
        transform.SetPosition(position);
        // Linear Damping
        linearVel = linearVel * frameLinearDamping;
        object->SetLinearVelocity(linearVel);

        // Orientation Stuff
        Quaternion orientation = transform.GetOrientation();
        Vector3 angVel = object->GetAngularVelocity();

        orientation = orientation + (Quaternion(angVel * dt * 0.5f, 0.0f) * orientation);
        orientation.Normalise();

        transform.SetOrientation(orientation);

        // Damp the angular velocity too
        float frameAngularDamping = 1.0f - (0.4f * dt);
        angVel = angVel * frameAngularDamping;
        object->SetAngularVelocity(angVel);
    }
}

/*
Once we're finished with a physics update, we have to
clear out any accumulated forces, ready to receive new
ones in the next 'game' frame.
*/
void PhysicsSystem::ClearForces()
{
    gameWorld.OperateOnContents(
        [](GameObject* o)
        {
            o->GetPhysicsObject()->ClearForces();
        });
}


/*

As part of the final physics tutorials, we add in the ability
to constrain objects based on some extra calculation, allowing
us to model springs and ropes etc.

*/
void PhysicsSystem::UpdateConstraints(float dt)
{
    std::vector<Constraint*>::const_iterator first;
    std::vector<Constraint*>::const_iterator last;
    gameWorld.GetConstraintIterators(first, last);

    for (auto i = first; i != last; ++i)
    {
        (*i)->UpdateConstraint(dt);
    }
}