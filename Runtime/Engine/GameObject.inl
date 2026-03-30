#pragma once

#include "GameObject.h"
#include "Components/TransformComponent.h"
namespace NLS::Engine
{
    template<typename T>
    T* GameObject::AddComponent(const std::function<void(Components::Component*)>& func)
    {
        static_assert(std::is_base_of_v<Components::Component, T>, "T must derive from Component");

        auto component = std::make_unique<T>();
        auto* typedComponent = component.get();

        if (!typedComponent)
            return nullptr;

        if (func)
            func(static_cast<Components::Component*>(typedComponent));

        m_vComponents.push_back(std::move(component));
        if constexpr (!std::is_same_v<T, Components::TransformComponent>)
        {
            typedComponent->CreateBy(this);
        }
        ComponentAddedEvent.Invoke(typedComponent);

        return typedComponent;
    }

    template<typename T>
    T* GameObject::GetComponent(bool includeSubType) const
    {
        for (const auto& component : m_vComponents)
        {
            if (!component)
                continue;

            if (includeSubType)
            {
                if (auto* match = dynamic_cast<T*>(component.get()))
                    return match;
                continue;
            }

            if (typeid(*component) == typeid(T))
                return static_cast<T*>(component.get());
        }

        return nullptr;
    }
}

