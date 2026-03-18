#include "Components/Component.h"
#include "GameObject.h"
using namespace NLS::Engine;
using namespace NLS::Engine::Components;

NLS::Engine::Components::Component::Component()
{

}

NLS::Engine::Components::Component::~Component()
{
}

void NLS::Engine::Components::Component::DestroyFromOwner()
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

void NLS::Engine::Components::Component::CreateBy(GameObject* owner)
{
	m_owner = owner;
    m_destroyedFromOwner = false;
	OnCreate();
}
