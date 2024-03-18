#include "GameObject.h"
#include "CollosionDetection/CollisionDetection.h"
#include "UDRefl/Object.hpp"
#include "UDRefl/ReflMngr.hpp"

using namespace NLS::Engine;
using namespace NLS::UDRefl;
GameObject::GameObject(string objectName)
{
    name = objectName;
    worldID = -1;
    isActive = true;
    boundingVolume = nullptr;
    physicsObject = nullptr;
    renderObject = nullptr;
    transform = AddComponent<Transform>();
}

GameObject::~GameObject()
{
    m_vComponents.clear();
    delete boundingVolume;
    delete physicsObject;
    delete renderObject;
}

bool GameObject::GetBroadphaseAABB(Vector3& outSize) const
{
    if (!boundingVolume)
    {
        return false;
    }
    outSize = broadphaseAABB;
    return true;
}

void GameObject::UpdateBroadphaseAABB()
{
    if (!boundingVolume)
    {
        return;
    }
    if (boundingVolume->type == VolumeType::AABB)
    {
        broadphaseAABB = ((AABBVolume&)*boundingVolume).GetHalfDimensions();
    }
    else if (boundingVolume->type == VolumeType::Sphere)
    {
        float r = ((SphereVolume&)*boundingVolume).GetRadius();
        broadphaseAABB = Vector3(r, r, r);
    }
    else if (boundingVolume->type == VolumeType::OBB)
    {
        Matrix3 mat = Matrix3(transform->GetOrientation());
        mat = mat.Absolute();
        Vector3 halfSizes = ((OBBVolume&)*boundingVolume).GetHalfDimensions();
        broadphaseAABB = mat * halfSizes;
    }
}

SharedObject GameObject::AddComponent(Type type)
{
    SharedObject component = Mngr.MakeShared(type);
    component.Invoke("CreateBy", TempArgsView{ this });
    m_vComponents.push_back(component);
    return component;
}


SharedObject GameObject::GetComponent(Type type)
{
    for (const auto component : m_vComponents)
    {
        auto targetType = component.GetType();
        if (targetType == type)
            return component;
    }
    return {};
}

