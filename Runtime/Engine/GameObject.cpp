#include "GameObject.h"
#include <algorithm>
#include "Components/TransformComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Components/SkyBoxComponent.h"
#include "Reflection/TypeCreator.h"
using namespace NLS::Engine;
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
m_transform(nullptr)
{
    m_transform = AddComponent<Components::TransformComponent>();
    if (m_transform)
    {
        m_transform->CreateBy(this);
    }
	CreatedEvent.Invoke(*this);
}

GameObject::~GameObject()
{
    for (auto& component : m_vComponents)
    {
        if (component)
            component->DestroyFromOwner();
    }
    m_vComponents.clear();
    m_transform = nullptr;
}

Component* GameObject::AddComponent(meta::Type type, const std::function<void(Component*)>& func)
{
    if (!type.IsValid() || !(type == NLS_TYPEOF(Component) || type.DerivesFrom(NLS_TYPEOF(Component))))
        return nullptr;

    if (type == NLS_TYPEOF(TransformComponent) && m_transform)
        return m_transform;

    const auto instance = meta::TypeCreator::CreateDynamic(type);
    if (!instance.IsValid())
        return nullptr;

    auto* component = instance.GetValue<Component*>();
    if (!component)
        return nullptr;

    if (func)
        func(component);

    m_vComponents.emplace_back(component);

    if (type == NLS_TYPEOF(TransformComponent))
    {
        m_transform = static_cast<TransformComponent*>(component);
    }
    else
    {
        component->CreateBy(this);
    }

    ComponentAddedEvent.Invoke(component);
    return component;
}


Component* GameObject::GetComponent(meta::Type type, bool includeSubType) const
{
    if (type == NLS_TYPEOF(TransformComponent))
        return GetComponent<TransformComponent>(includeSubType);
    if (type == NLS_TYPEOF(CameraComponent))
        return GetComponent<CameraComponent>(includeSubType);
    if (type == NLS_TYPEOF(LightComponent))
        return GetComponent<LightComponent>(includeSubType);
    if (type == NLS_TYPEOF(MaterialRenderer))
        return GetComponent<MaterialRenderer>(includeSubType);
    if (type == NLS_TYPEOF(MeshRenderer))
        return GetComponent<MeshRenderer>(includeSubType);
    if (type == NLS_TYPEOF(SkyBoxComponent))
        return GetComponent<SkyBoxComponent>(includeSubType);
    return nullptr;
}

bool Engine::GameObject::RemoveComponent(Component* component)
{
    if (!component || component == m_transform)
        return false;

    for (auto it = m_vComponents.begin(); it != m_vComponents.end(); ++it)
    {
        if (it->get() == component)
        {
            component->DestroyFromOwner();
            m_vComponents.erase(it);
            ComponentRemovedEvent.Invoke(nullptr);
            return true;
        }
    }
    return false;
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
	std::for_each(m_vComponents.begin(), m_vComponents.end(), [](auto& element)
    {
        if (auto* component = element.get())
            component->OnAwake();
    });
}

void GameObject::OnStart()
{
	m_started = true;
	std::for_each(m_vComponents.begin(), m_vComponents.end(), [](auto& element)
    {
        if (auto* component = element.get())
            component->OnStart();
    });
}

void GameObject::OnEnable()
{
	std::for_each(m_vComponents.begin(), m_vComponents.end(), [](auto& element)
    {
        if (auto* component = element.get())
            component->OnEnable();
    });
}

void GameObject::OnDisable()
{
	std::for_each(m_vComponents.begin(), m_vComponents.end(), [](auto& element)
    {
        if (auto* component = element.get())
            component->OnDisable();
    });
}

void GameObject::OnDestroy()
{
	std::for_each(m_vComponents.begin(), m_vComponents.end(), [](auto& element)
    {
        if (auto* component = element.get())
            component->OnDestroy();
    });
}

void GameObject::OnUpdate(float p_deltaTime)
{
	if (IsActive())
	{
		std::for_each(m_vComponents.begin(), m_vComponents.end(), [&](auto& element)
        {
            if (auto* component = element.get())
                component->OnUpdate(p_deltaTime);
        });
	}
}

void GameObject::OnFixedUpdate(float p_deltaTime)
{
	if (IsActive())
	{
		std::for_each(m_vComponents.begin(), m_vComponents.end(), [&](auto& element)
        {
            if (auto* component = element.get())
                component->OnFixedUpdate(p_deltaTime);
        });
	}
}

void GameObject::OnLateUpdate(float p_deltaTime)
{
	if (IsActive())
	{
		std::for_each(m_vComponents.begin(), m_vComponents.end(), [&](auto& element)
        {
            if (auto* component = element.get())
                component->OnLateUpdate(p_deltaTime);
        });
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

