#include "GameObject.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include "Components/TransformComponent.h"
#include "PrimitiveFactory.h"
#include "Reflection/TypeCreator.h"
#include "SceneSystem/Scene.h"

namespace NLS::Engine
{
namespace
{
// Component callbacks may remove themselves or add other components. Resolve
// IDs at each callback boundary instead of keeping iterators into ownership.
template<class Fn> void VisitComponents(GameObject& owner, Fn fn)
{
    // Most objects have only a few components. Keep their ID snapshot inline,
    // retaining the same callback mutation/lifetime checks as the heap path.
    const auto& components = owner.GetComponents();
    std::array<NLS::InstanceID, 8> localIds;
    std::vector<NLS::InstanceID> overflow;
    if (components.size() > localIds.size()) overflow.resize(components.size());
    const auto storage = overflow.empty() ? std::span(localIds) : std::span(overflow);
    size_t count = 0;
    for (const auto& component : components)
        if (component) storage[count++] = component->GetInstanceID();
    const auto ids = storage.first(count);
    for (auto id : ids)
        if (auto* component = dynamic_cast<Components::Component*>(NLS::Object::IDToPointerNoThreadCheck(id));
            component && component->gameobject() == &owner) fn(*component);
}
}

using namespace Components;

Event<GameObject&> GameObject::DestroyedEvent;
Event<GameObject&> GameObject::MarkedDestroyEvent;
Event<GameObject&> GameObject::CreatedEvent;
Event<GameObject&, GameObject&> GameObject::AttachEvent;
Event<GameObject&> GameObject::DettachEvent;

GameObject::GameObject(const std::string& p_name, const std::string& p_tag):
name(p_name),
tag(p_tag.empty() ? "Untagged" : p_tag),
active(true),
m_transform(nullptr)
{
    m_transform = AddComponent<Components::TransformComponent>();
    if (m_transform)
    {
        m_transform->CreateBy(this);
    }
	CreatedEvent.Invoke(*this);
}

GameObject::GameObject(SilentCreationTag, const std::string& p_name, const std::string& p_tag):
name(p_name),
tag(p_tag.empty() ? "Untagged" : p_tag),
active(true),
m_transform(nullptr)
{
    m_transform = AddComponent<Components::TransformComponent>();
    if (m_transform)
    {
        m_transform->CreateBy(this);
    }
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
    if (!type.IsValid())
        return nullptr;

    const auto componentBaseType = NLS_TYPEOF(Component);
    if (type != componentBaseType && !type.DerivesFrom(componentBaseType))
        return nullptr;

    for (const auto& component : m_vComponents)
    {
        if (!component)
            continue;

        const auto componentType = component->GetType();
        if (!componentType.IsValid())
            continue;

        if (componentType == type)
            return component.get();

        if (includeSubType && componentType.DerivesFrom(type))
            return component.get();
    }

    return nullptr;
}

bool GameObject::RemoveComponent(Component* component)
{
    if (!component || component == m_transform)
        return false;

    for (auto it = m_vComponents.begin(); it != m_vComponents.end(); ++it)
    {
        if (it->get() == component)
        {
            auto removedComponent = std::move(*it);
            m_vComponents.erase(it);
            component->DestroyFromOwner();
            ComponentRemovedEvent.Invoke(component);
            return true;
        }
    }
    return false;
}

bool GameObject::MoveComponent(Component* component, size_t index)
{
    if (!component || index >= m_vComponents.size())
        return false;

    auto found = std::find_if(m_vComponents.begin(), m_vComponents.end(), [component](const auto& entry)
    {
        return entry.get() == component;
    });

    if (found == m_vComponents.end())
        return false;

    auto owned = std::move(*found);
    m_vComponents.erase(found);
    m_vComponents.insert(m_vComponents.begin() + static_cast<std::ptrdiff_t>(index), std::move(owned));
    return true;
}

void GameObject::SetActive(bool p_active)
{
	if (p_active != active)
	{
		RecursiveWasActiveUpdate();
		active = p_active;
        MarkRenderStateChanged();
		RecursiveActiveUpdate();
	}
}

void GameObject::MarkRenderStateChanged()
{
    ++m_renderStateRevision;
    if (m_scene != nullptr)
        m_scene->MarkRenderContentChanged();
}

GameObject* GameObject::CreatePrimitive(const std::string& p_typeName)
{
    if (m_scene == nullptr || m_destroyed)
        return nullptr;

    std::string requested = p_typeName.empty() ? "Cube" : p_typeName;
    for (auto& character : requested)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

    const auto primitiveType = TryGetPrimitiveTypeFromMeshResourcePath(
        std::string("builtin:Primitive/") + requested);
    if (!primitiveType.has_value())
        return nullptr;

    auto& primitive = m_scene->CreateGameObject(GetPrimitiveName(*primitiveType));
    primitive.SetParent(*this);
    if (!ConfigurePrimitiveGameObject(primitive, *primitiveType))
    {
        m_scene->DestroyGameObject(primitive);
        return nullptr;
    }
    return &primitive;
}

bool GameObject::IsSelfActive() const
{
	return active;
}

bool GameObject::IsActive() const
{
	return active && (m_parent ? m_parent->IsActive() : true);
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

void GameObject::MarkAsDestroy()
{
    if (m_destroyed)
        return;

    m_destroyed = true;
    MarkRenderStateChanged();
    MarkedDestroyEvent.Invoke(*this);

    for (auto child : m_children)
        child->MarkAsDestroy();
}

std::vector<GameObject*>& GameObject::GetChildren()
{
    return m_children;
}

const std::vector<GameObject*>& GameObject::GetChildren() const
{
    return m_children;
}

GameObject* GameObject::GetParent() const
{
    return m_parent;
}

bool GameObject::HasParent() const
{
    return m_parent;
}

bool GameObject::IsDescendantOf(const GameObject* p_actor) const
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

void GameObject::DetachFromParent()
{
    DettachEvent.Invoke(*this);

    /* Remove the actor from the parent children list */
    if (m_parent)
    {
        m_parent->m_children.erase(
            std::remove(m_parent->m_children.begin(), m_parent->m_children.end(), this),
            m_parent->m_children.end());
    }

    m_parent = nullptr;

    m_transform->RemoveParent();
    MarkRenderStateChanged();
}

void GameObject::SetParent(GameObject& p_parent)
{
    DetachFromParent();

    /* Define the given parent as the new parent */
    m_parent = &p_parent;
    m_transform->SetParent(*p_parent.m_transform);
    MarkRenderStateChanged();

    /* Store the actor in the parent children list */
    if (std::find(p_parent.m_children.begin(), p_parent.m_children.end(), this) == p_parent.m_children.end())
        p_parent.m_children.push_back(this);

    AttachEvent.Invoke(*this, p_parent);
}

void GameObject::OnAwake()
{
    m_awaked = true;
    VisitComponents(*this, [](Component& component) { component.RuntimeAwake(); });
}
void GameObject::OnEnable()
{
    VisitComponents(*this, [](Component& component) { component.RuntimeEnable(); });
}
void GameObject::OnStart()
{
    m_started = true;
    VisitComponents(*this, [](Component& component) { component.RuntimeStart(); });
}
void GameObject::OnDisable()
{
    VisitComponents(*this, [](Component& component) { component.RuntimeDisable(); });
}
void GameObject::OnDestroy()
{
    VisitComponents(*this, [](Component& component) { component.RuntimeDestroy(); });
}
void GameObject::OnUpdate(float delta)
{
    if (!IsActive()) return;
    OnAwake(); OnEnable(); OnStart();
    VisitComponents(*this, [delta](Component& component) { if (component.IsActiveAndEnabled()) component.OnUpdate(delta); });
}
void GameObject::OnFixedUpdate(float delta)
{
    if (!IsActive()) return;
    OnAwake(); OnEnable(); OnStart();
    VisitComponents(*this, [delta](Component& component) { if (component.IsActiveAndEnabled()) component.OnFixedUpdate(delta); });
}
void GameObject::OnLateUpdate(float delta)
{
    if (!IsActive()) return;
    VisitComponents(*this, [delta](Component& component) { if (component.IsActiveAndEnabled()) component.OnLateUpdate(delta); });
}

void GameObject::RecursiveActiveUpdate()
{
	bool isActive = IsActive();

	if (!m_sleeping)
	{
		if (!m_wasActive && isActive)
		{
			if (!m_awaked)
				OnAwake();

			OnEnable();
		}

		if (m_wasActive && !isActive)
			OnDisable();
	}

	for (auto child : m_children)
		child->RecursiveActiveUpdate();
}

void GameObject::RecursiveWasActiveUpdate()
{
	m_wasActive = IsActive();
	for (auto child : m_children)
		child->RecursiveWasActiveUpdate();
}
} // namespace NLS::Engine

