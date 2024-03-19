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
    renderObject = nullptr;
    transform = AddComponent<Transform>();
}

GameObject::~GameObject()
{
    m_vComponents.clear();
    delete renderObject;
}

bool GameObject::GetBroadphaseAABB(Vector3& outSize) const
{
    if (!GetComponent<CollisionVolume>())
    {
        return false;
    }
    outSize = broadphaseAABB;
    return true;
}

void GameObject::UpdateBroadphaseAABB()
{
    if (!GetComponent<CollisionVolume>())
    {
        return;
    }
    if (GetComponent<CollisionVolume>()->type == VolumeType::AABB)
    {
        broadphaseAABB = ((AABBVolume&)*GetComponent<CollisionVolume>()).GetHalfDimensions();
    }
    else if (GetComponent<CollisionVolume>()->type == VolumeType::Sphere)
    {
        float r = ((SphereVolume&)*GetComponent<CollisionVolume>()).GetRadius();
        broadphaseAABB = Vector3(r, r, r);
    }
    else if (GetComponent<CollisionVolume>()->type == VolumeType::OBB)
    {
        Matrix3 mat = Matrix3(transform->GetOrientation());
        mat = mat.Absolute();
        Vector3 halfSizes = ((OBBVolume&)*GetComponent<CollisionVolume>()).GetHalfDimensions();
        broadphaseAABB = mat * halfSizes;
    }
}

SharedObject GameObject::AddComponent(Type type, const std::function<void(Component*)>& func)
{
    SharedObject component = Mngr.MakeShared(type);
    if (func)
    {
        func(component.StaticCast_DerivedToBase(Type_of<Component>).AsPtr<Component>());
    }
    component.Invoke("CreateBy", TempArgsView{ this });
    m_vComponents.push_back(component);
    return component;
}


SharedObject GameObject::GetComponent(Type type, bool includeSubType) const
{
    for (const auto component : m_vComponents)
    {
        auto targetType = component.GetType();
        if (targetType == type || (includeSubType && Mngr.IsDerivedFrom(targetType, type)))
            return component;
    }
    return {};
}

