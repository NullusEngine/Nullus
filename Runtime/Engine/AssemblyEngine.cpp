#include "AssemblyEngine.h"
#include "Component.h"
#include "UDRefl/ReflMngr.hpp"
#include "Transform.h"
#include "Physics/PhysicsObject.h"
#include "CollosionDetection/SphereVolume.h"
#include "CollosionDetection/AABBVolume.h"
#include "CollosionDetection/OBBVolume.h"
#include "CollosionDetection/CapsuleVolume.h"
namespace NLS
{
namespace Engine
{
    using namespace UDRefl;
void AssemblyEngine::Initialize()
{
    Mngr.RegisterType<Component>();
    Mngr.AddMethod<&Component::CreateBy>("CreateBy");

    Mngr.RegisterType<Transform>();
    Mngr.AddBases<Transform, Component>();

    Mngr.RegisterType<PhysicsObject>();
    Mngr.AddBases<PhysicsObject, Component>();

    Mngr.RegisterType<CollisionVolume>();
    Mngr.AddBases<CollisionVolume, Component>();

    Mngr.RegisterType<SphereVolume>();
    Mngr.AddBases<SphereVolume, CollisionVolume>();

    Mngr.RegisterType<AABBVolume>();
    Mngr.AddBases<AABBVolume, CollisionVolume>();

    Mngr.RegisterType<OBBVolume>();
    Mngr.AddBases<OBBVolume, CollisionVolume>();

    Mngr.RegisterType<CapsuleVolume>();
    Mngr.AddBases<CapsuleVolume, CollisionVolume>();
}
} // namespace Engine
} // namespace NLS