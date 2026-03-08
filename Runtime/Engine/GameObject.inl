#pragma once

#include "GameObject.h"
#include "Reflection/Compat/ReflMngr.hpp"
#include <utility>
namespace NLS::Engine
{
    template<typename T>
    T* GameObject::AddComponent(const std::function<void(Components::Component*)>& func)
    {
        static_assert(std::is_base_of_v<Components::Component, T>, "T must derive from Component");

        auto component = AddComponent(UDRefl::Type_of<T>, func);
        if (!component)
            return nullptr;

        return component.StaticCast(UDRefl::Type_of<T>).template AsPtr<T>();
    }

    template<typename T>
    T* GameObject::GetComponent(bool includeSubType) const
    {
        return GetComponent(UDRefl::Type_of<T>, includeSubType).StaticCast(UDRefl::Type_of<T>).template AsPtr<T>();
    }
}

