#include "GameObject.h"
#include "UDRefl/Object.hpp"
#include "UDRefl/ReflMngr.hpp"

using namespace NLS::Engine;
using namespace NLS::UDRefl;
GameObject::GameObject(string objectName)
{
    name = objectName;
    worldID = -1;
    isActive = true;
    AddComponent<TransformComponent>();
}

GameObject::~GameObject()
{
    m_vComponents.clear();
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

