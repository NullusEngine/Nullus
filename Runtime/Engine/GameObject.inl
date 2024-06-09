#pragma once

#include "GameObject.h"
#include "UDRefl/ReflMngr.hpp"
namespace NLS::Engine
{
    template<typename T>
    T* GameObject::AddComponent(const std::function<void(Components::Component*)>& func)
    {
        return AddComponent(Type_of<T>, func).template AsPtr<T>();
    }

    template<typename T>
    T* GameObject::GetComponent(bool includeSubType) const
    {
        return GetComponent(Type_of<T>, includeSubType).StaticCast(Type_of<T>).template AsPtr<T>();
    }
}

