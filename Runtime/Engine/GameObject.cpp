#include "GameObject.h"
#include "UDRefl/Object.hpp"
#include "UDRefl/ReflMngr.hpp"
#include <algorithm>
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

