#include "PlayerInputComponent.h"
#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>
#include <algorithm>

namespace NLS::Engine::Components
{
PlayerInputComponent::~PlayerInputComponent() { Detach(); }
void PlayerInputComponent::OnAwake()
{
    if (!Core::ServiceLocator::Contains<Input::InputDomain>()) return;
    m_domain = &Core::ServiceLocator::Get<Input::InputDomain>();
    m_domainLifetime = m_domain->GetLifetime();
    auto users = m_domain->GetUsers();
    auto user = users.size() == 1 && !users[0]->GetDefinition() && users[0]->IsEnabled() ? users[0] : m_domain->CreateUser();
    user->SetEnabled(false);
    m_user = user;
    RefreshDevices();
    RefreshDefinition();
}
void PlayerInputComponent::RefreshDevices()
{
    auto user = m_user.lock(); if (!user || m_domainLifetime.expired()) return;
    auto& system = m_domain->GetInputSystem();
    if (m_appliedKeyboard != static_cast<int>(m_keyboardAndMouse))
    {
        if (m_keyboardAndMouse) { user->PairDevice(system.GetKeyboard()); user->PairDevice(system.GetMouse()); }
        else { user->UnpairDevice(system.GetKeyboard()); user->UnpairDevice(system.GetMouse()); }
        m_appliedKeyboard = m_keyboardAndMouse;
    }
    Input::DeviceId candidate; int index = 0;
    for (const auto& device : system.GetDevices()) if (device.connected && device.descriptor.kind == Input::DeviceKind::Gamepad)
        if (index++ == m_gamepadIndex) { candidate = device.id; break; }
    if (candidate != m_pairedGamepad)
    {
        if (m_pairedGamepad) user->UnpairDevice(m_pairedGamepad);
        if (candidate) user->PairDevice(candidate);
        m_pairedGamepad = candidate;
    }
}
bool PlayerInputComponent::RefreshDefinition()
{
    auto user = m_user.lock();
    if (!user || m_domainLifetime.expired()) return false;
    Serialize::ObjectIdentifier identifier;
    if (!Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(m_actions.GetInstanceID(), identifier))
    { m_error = "Assign an Input Actions asset"; return false; }
    auto asset = Assets::LoadInputActionsAsset(identifier, m_error);
    if (!asset) return m_asset && user->GetDefinition(); // invalid reload retains the last valid mapping
    m_asset = std::move(asset);
    const auto definition = m_asset->GetDefinition();
    const auto context = m_defaultContext.IsValid() ? m_defaultContext : definition->GetDefinition().defaultContext;
    if (context.IsValid() && definition->FindContext(context) == SIZE_MAX) { m_error = "Configured default ContextId is absent"; return false; }
    if (!user->GetDefinition())
    {
        user->SetDefinition(definition); if (context.IsValid()) user->PushContext(context); m_appliedContext = context;
    }
    else if ((user->GetDefinition() != definition && m_requestedDefinition != definition) || context != m_appliedContext)
    {
        const auto previous = m_appliedContext; m_requestedDefinition = definition; m_appliedContext = context;
        auto* domain = m_domain; std::weak_ptr<Input::InputUser> weakUser = user;
        domain->EnqueueChange([domain, weakUser, definition, context, previous]
        {
            const auto target = weakUser.lock(); const auto users = domain->GetUsers();
            if (!target || std::find(users.begin(), users.end(), target) == users.end()) return;
            if (target->GetDefinition() != definition) target->SetDefinition(definition);
            if (previous.IsValid() && previous != context) target->PopContext(previous);
            if (context.IsValid() && context != previous) target->PushContext(context);
        });
    }
    else if (user->GetDefinition() == definition) m_requestedDefinition.reset();
    m_error.clear();
    return true;
}
void PlayerInputComponent::OnEnable()
{
    if (auto user = m_user.lock(); user && RefreshDefinition()) user->SetEnabled(true);
}
void PlayerInputComponent::OnDisable() { if (auto user = m_user.lock()) user->SetEnabled(false); }
void PlayerInputComponent::OnDestroy() { Detach(); }
void PlayerInputComponent::OnUpdate(float)
{
    auto user = m_user.lock();
    if (!user) return;
    RefreshDevices();
    const bool wasFailed = !m_error.empty();
    if (!RefreshDefinition()) user->SetEnabled(false);
    else if (wasFailed && IsActiveAndEnabled()) user->SetEnabled(true);
}
void PlayerInputComponent::SetActions(Serialize::PPtr<Assets::InputActionsAsset> actions)
{
    m_actions = actions;
    m_asset.reset();
    m_requestedDefinition.reset();
    if (auto user = m_user.lock())
    {
        user->SetEnabled(IsActiveAndEnabled() && RefreshDefinition());
    }
}
void PlayerInputComponent::Detach()
{
    if (auto user = m_user.lock())
    {
        user->SetEnabled(false);
        if (m_domain && !m_domainLifetime.expired()) m_domain->RemoveUser(user->GetId());
    }
    m_user.reset(); m_asset.reset(); m_requestedDefinition.reset(); m_domain = nullptr; m_domainLifetime.reset();
}
}
