#include "Components/Component.h"
#include "GameObject.h"
using namespace NLS::Engine;
using namespace NLS::Engine::Components;

NLS::Engine::Components::Component::Component()
{

}

NLS::Engine::Components::Component::~Component()
{
    if (m_owner && m_owner->IsActive())
	{
		OnDisable();
		OnDestroy();
	}
}

void NLS::Engine::Components::Component::CreateBy(GameObject* owner)
{
	m_owner = owner;
	OnCreate();
}
#include "UDRefl/ReflMngr.hpp"
using namespace NLS::UDRefl;
void Component::Bind()
{
    Mngr.RegisterType<Component>();
    Mngr.AddMethod<&Component::CreateBy>("CreateBy");
}
