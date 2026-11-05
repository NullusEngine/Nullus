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

    if (!m_runtimeDestroyed && m_owner && m_owner->IsActive() && m_enabled)
        RuntimeDisable();

    RuntimeDestroy();
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
    {
        if (m_runtimeAwake) RuntimeDisable(); else OnDisable();
    }
    else if (!wasActive && isActive)
    {
        if (m_runtimeAwake) RuntimeEnable(); else OnEnable();
    }
}

void Component::RuntimeAwake()
{
    if (m_runtimeAwake || m_runtimeDestroyed) return;
    m_runtimeAwake = true;
    OnAwake();
}
void Component::RuntimeEnable()
{
    if (!m_runtimeAwake || m_runtimeDestroyed || m_runtimeEnabled || !IsActiveAndEnabled()) return;
    m_runtimeEnabled = true;
    OnEnable();
}
void Component::RuntimeStart()
{
    if (!m_runtimeEnabled || m_runtimeStarted || m_runtimeDestroyed || !IsActiveAndEnabled()) return;
    m_runtimeStarted = true;
    OnStart();
}
void Component::RuntimeDisable()
{
    if (!m_runtimeEnabled || m_runtimeDestroyed) return;
    m_runtimeEnabled = false;
    OnDisable();
}
void Component::RuntimeDestroy()
{
    if (m_runtimeDestroyed) return;
    m_runtimeDestroyed = true;
    OnDestroy();
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
