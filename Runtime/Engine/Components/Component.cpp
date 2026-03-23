#include "Components/Component.h"
#include "GameObject.h"

namespace NLS::Engine::Components
{
Component::Component()
{
}

Component::~Component()
{
}

void Component::DestroyFromOwner()
{
    if (m_destroyedFromOwner)
    {
        m_owner = nullptr;
        return;
    }

    if (m_owner && m_owner->IsActive())
        OnDisable();

    OnDestroy();
    m_destroyedFromOwner = true;
    m_owner = nullptr;
}

void Component::CreateBy(GameObject* owner)
{
	m_owner = owner;
    m_destroyedFromOwner = false;
	OnCreate();
}
} // namespace NLS::Engine::Components
