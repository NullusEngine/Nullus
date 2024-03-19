#pragma once
#include "Transform.h"
#include "CollosionDetection/CollisionVolume.h"

#include "Engine/Physics/PhysicsObject.h"
#include "RenderObject.h"

#include <vector>
#include "EngineDef.h"
#include "Component.h"
#include "UTemplate/Type.hpp"
#include "UDRefl/Object.hpp"
using std::vector;

namespace NLS
{
namespace Engine
{
using namespace UDRefl;
class NLS_ENGINE_API GameObject
{
public:
    GameObject(string name = "");
    ~GameObject();
    template<typename T>
    T* AddComponent(const std::function<void(Component*)>& func = {});
    template<typename T>
    T* GetComponent(bool includeSubType = true) const;
    SharedObject AddComponent(Type type, const std::function<void(Component*)>& func = {});
    SharedObject GetComponent(Type type, bool includeSubType = true) const;
    bool IsActive() const
    {
        return isActive;
    }

    Transform& GetTransform()
    {
        return *transform;
    }

    RenderObject* GetRenderObject() const
    {
        return renderObject;
    }

    void SetRenderObject(RenderObject* newObject)
    {
        renderObject = newObject;
    }

    const string& GetName() const
    {
        return name;
    }

    virtual void OnCollisionBegin(GameObject* otherObject)
    {
        // std::cout << "OnCollisionBegin event occured!\n";
    }

    virtual void OnCollisionEnd(GameObject* otherObject)
    {
        // std::cout << "OnCollisionEnd event occured!\n";
    }

    bool GetBroadphaseAABB(Vector3& outsize) const;

    void UpdateBroadphaseAABB();

    void SetWorldID(int newID)
    {
        worldID = newID;
    }

    int GetWorldID() const
    {
        return worldID;
    }

protected:
    std::vector<SharedObject> m_vComponents;
    Transform* transform;
    RenderObject* renderObject;

    bool isActive;
    int worldID;
    string name;

    Vector3 broadphaseAABB;
};

template<typename T>
T* GameObject::AddComponent(const std::function<void(Component*)>& func)
{
    return AddComponent(Type_of<T>, func).AsPtr<T>();
}


template<typename T>
T* GameObject::GetComponent(bool includeSubType) const
{
    return GetComponent(Type_of<T>, includeSubType).StaticCast(Type_of<T>).AsPtr<T>();
}

} // namespace Engine
} // namespace NLS
