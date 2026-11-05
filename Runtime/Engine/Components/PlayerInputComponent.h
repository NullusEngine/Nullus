#pragma once
#include "Components/Component.h"
#include "Assets/InputActionsAsset.h"
#include <Input/InputDomain.h>
#include "Components/PlayerInputComponent.generated.h"
#include <memory>

namespace NLS::Engine::Components
{
CLASS(NLS_ENGINE_API PlayerInputComponent, ComponentMenu("Input/Player Input"), DisallowMultipleComponent) : public Component
{
public:
    GENERATED_BODY()
    ~PlayerInputComponent() override;
    void OnAwake() override;
    void OnEnable() override;
    void OnDisable() override;
    void OnDestroy() override;
    void OnUpdate(float deltaTime) override;
    FUNCTION() Serialize::PPtr<Assets::InputActionsAsset> GetActions() const { return m_actions; }
    FUNCTION() void SetActions(Serialize::PPtr<Assets::InputActionsAsset> actions);
    std::shared_ptr<Input::InputUser> GetInputUser() const { return m_user.lock(); }
    const std::string& GetLastError() const { return m_error; }
private:
    bool RefreshDefinition();
    void RefreshDevices();
    void Detach();
    PROPERTY(Getter(GetActions), Setter(SetActions)) Serialize::PPtr<Assets::InputActionsAsset> m_actions;
    PROPERTY() Guid m_defaultContext;
    PROPERTY() bool m_keyboardAndMouse = true;
    PROPERTY(Range(-1, 15)) int m_gamepadIndex = -1;
    std::weak_ptr<Input::InputUser> m_user;
    Input::InputDomain* m_domain = nullptr;
    std::weak_ptr<const void> m_domainLifetime;
    std::shared_ptr<Assets::InputActionsAsset> m_asset;
    std::shared_ptr<const Input::CompiledInput> m_requestedDefinition;
    Input::ContextId m_appliedContext;
    Input::DeviceId m_pairedGamepad;
    int m_appliedKeyboard = -1;
    std::string m_error;
};
}
