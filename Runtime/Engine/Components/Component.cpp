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

    if (m_owner && m_owner->IsActive() && m_enabled)
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

void Component::SetEnabled(bool enabled)
{
    if (m_enabled == enabled || m_destroyedFromOwner)
        return;
    const bool wasActive = IsActiveAndEnabled();
    m_enabled = enabled;
    const bool isActive = IsActiveAndEnabled();
    if (wasActive && !isActive)
        OnDisable();
    else if (!wasActive && isActive)
        OnEnable();
}

bool Component::IsActiveAndEnabled() const
{
    return m_enabled && m_owner != nullptr && m_owner->IsActive();
}

void Component::MarkRenderStateChanged()
{
    if (m_owner != nullptr)
        m_owner->MarkRenderStateChanged();
}
} // namespace NLS::Engine::Components
