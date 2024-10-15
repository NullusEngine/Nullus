#include "GameObject.h"
#include "UDRefl/Object.hpp"
#include "UDRefl/ReflMngr.hpp"
#include <algorithm>
#include "Components/TransformComponent.h"
using namespace NLS::Engine;
using namespace NLS::UDRefl;
using namespace NLS::Engine::Components;
using namespace NLS;

Event<GameObject&>GameObject::DestroyedEvent;
Event<GameObject&> GameObject::CreatedEvent;
Event<GameObject&, GameObject&> GameObject::AttachEvent;
Event<GameObject&> GameObject::DettachEvent;

GameObject::GameObject(int64_t p_actorID, const std::string& p_name, const std::string& p_tag, bool& p_playing):
	m_worldID(p_actorID),
m_name(p_name),
m_tag(p_tag),
m_playing(p_playing),
m_active(true),
m_transform(AddComponent<Components::TransformComponent>())
{
	CreatedEvent.Invoke(*this);
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
    ComponentAddedEvent.Invoke(component);
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

bool Engine::GameObject::RemoveComponent(SharedObject component)
{
    for (auto it = m_vComponents.begin(); it != m_vComponents.end(); ++it)
    {
        if (*it == component)
        {
            ComponentRemovedEvent.Invoke(component);
            m_vComponents.erase(it);
            return true;
        }
    }
    return false;
}

#include "UDRefl/ReflMngr.hpp"
using namespace NLS::UDRefl;
void Engine::GameObject::Bind()
{
    Mngr.RegisterType<GameObject>();
}

void NLS::Engine::GameObject::SetActive(bool p_active)
{
	if (p_active != m_active)
	{
		RecursiveWasActiveUpdate();
		m_active = p_active;
		RecursiveActiveUpdate();
	}
}

bool NLS::Engine::GameObject::IsSelfActive() const
{
	return m_active;
}

bool NLS::Engine::GameObject::IsActive() const
{
	return m_active && (m_parent ? m_parent->IsActive() : true);
}

void NLS::Engine::GameObject::OnCollisionEnter(GameObject& p_otherObject)
{
}

void NLS::Engine::GameObject::OnCollisionStay(GameObject& p_otherObject)
{
}

void NLS::Engine::GameObject::OnCollisionExit(GameObject& p_otherObject)
{
}

void NLS::Engine::GameObject::OnTriggerEnter(GameObject& p_otherObject)
{
}

void NLS::Engine::GameObject::OnTriggerStay(GameObject& p_otherObject)
{
}

void NLS::Engine::GameObject::OnTriggerExit(GameObject& p_otherObject)
{
}

void Engine::GameObject::MarkAsDestroy()
{
    m_destroyed = true;

    for (auto child : m_children)
        child->MarkAsDestroy();
}

std::vector<GameObject*>& Engine::GameObject::GetChildren()
{
    return m_children;
}

int64_t Engine::GameObject::GetParentID() const
{
    return m_parentID;
}

GameObject* Engine::GameObject::GetParent() const
{
    return m_parent;
}

bool Engine::GameObject::HasParent() const
{
    return m_parent;
}

bool Engine::GameObject::IsDescendantOf(const GameObject* p_actor) const
{
    const GameObject* currentParentActor = m_parent;

    while (currentParentActor != nullptr)
    {
        if (currentParentActor == p_actor)
        {
            return true;
        }
        currentParentActor = currentParentActor->GetParent();
    }

    return false;
}

void Engine::GameObject::DetachFromParent()
{
    DettachEvent.Invoke(*this);

    /* Remove the actor from the parent children list */
    if (m_parent)
    {
        m_parent->m_children.erase(std::remove_if(m_parent->m_children.begin(), m_parent->m_children.end(), [this](GameObject* p_element)
                                                  { return p_element == this; }));
    }

    m_parent = nullptr;
    m_parentID = 0;

    m_transform->RemoveParent();
}

void Engine::GameObject::SetParent(GameObject& p_parent)
{
    DetachFromParent();

    /* Define the given parent as the new parent */
    m_parent = &p_parent;
    m_parentID = p_parent.m_worldID;
    m_transform->SetParent(*p_parent.m_transform);

    /* Store the actor in the parent children list */
    p_parent.m_children.push_back(this);

    AttachEvent.Invoke(*this, p_parent);
}

void GameObject::OnAwake()
{
	m_awaked = true;
	std::for_each(m_vComponents.begin(), m_vComponents.end(), [](auto element) { element.Invoke("OnAwake"); });
}

void GameObject::OnStart()
{
	m_started = true;
	std::for_each(m_vComponents.begin(), m_vComponents.end(), [](auto element) { element.Invoke("OnStart"); });
}

void GameObject::OnEnable()
{
	std::for_each(m_vComponents.begin(), m_vComponents.end(), [](auto element) { element.Invoke("OnEnable"); });
}

void GameObject::OnDisable()
{
	std::for_each(m_vComponents.begin(), m_vComponents.end(), [](auto element) { element.Invoke("OnDisable"); });
}

void GameObject::OnDestroy()
{
	std::for_each(m_vComponents.begin(), m_vComponents.end(), [](auto element) { element.Invoke("OnDestroy"); });
}

void GameObject::OnUpdate(float p_deltaTime)
{
	if (IsActive())
	{
		std::for_each(m_vComponents.begin(), m_vComponents.end(), [&](auto element) { element.Invoke("OnUpdate", TempArgsView{ p_deltaTime }); });
	}
}

void GameObject::OnFixedUpdate(float p_deltaTime)
{
	if (IsActive())
	{
		std::for_each(m_vComponents.begin(), m_vComponents.end(), [&](auto element) { element.Invoke("OnFixedUpdate", TempArgsView{ p_deltaTime }); });
	}
}

void GameObject::OnLateUpdate(float p_deltaTime)
{
	if (IsActive())
	{
		std::for_each(m_vComponents.begin(), m_vComponents.end(), [&](auto element) { element.Invoke("OnLateUpdate", TempArgsView{ p_deltaTime }); });
	}
}

void NLS::Engine::GameObject::RecursiveActiveUpdate()
{
	bool isActive = IsActive();

	if (!m_sleeping)
	{
		if (!m_wasActive && isActive)
		{
			if (!m_awaked)
				OnAwake();

			OnEnable();

			if (!m_started)
				OnStart();
		}

		if (m_wasActive && !isActive)
			OnDisable();
	}

	for (auto child : m_children)
		child->RecursiveActiveUpdate();
}

void NLS::Engine::GameObject::RecursiveWasActiveUpdate()
{
	m_wasActive = IsActive();
	for (auto child : m_children)
		child->RecursiveWasActiveUpdate();
}

