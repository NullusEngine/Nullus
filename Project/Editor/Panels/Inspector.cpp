#include "Panels/Inspector.h"

#include <UI/Widgets/Texts/Text.h>
#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Drags/DragMultipleFloats.h>
#include <UI/Widgets/Drags/DragFloat.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Visual/Image.h>
#include <UI/Widgets/InputFields/InputFloat.h>
#include <UI/Widgets/Selection/ColorEdit.h>
#include <UI/Plugins/DDTarget.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Layout/GroupCollapsable.h>

#include <ServiceLocator.h>
#include <ResourceManagement/ModelManager.h>
#include <ResourceManagement/TextureManager.h>
#include <ResourceManagement/ShaderManager.h>

#include <Windowing/Dialogs/MessageBox.h>

#include "Core/EditorActions.h"
//#include <UI/GUIDrawer.h>
#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Components/LightComponent.h"
#include "Components/CameraComponent.h"
#include "GameObject.h"
namespace NLS::Editor::Panels
{
Inspector::Inspector(const std::string& p_title, bool p_opened, const NLS::UI::Settings::PanelWindowSettings& p_windowSettings)
    : NLS::UI::Panels::PanelWindow(p_title, p_opened, p_windowSettings)
{
    m_inspectorHeader = &CreateWidget<UI::Widgets::Layout::Group>();
    m_inspectorHeader->enabled = false;
    m_actorInfo = &CreateWidget<UI::Widgets::Layout::Group>();

    auto& headerColumns = m_inspectorHeader->CreateWidget<UI::Widgets::Layout::Columns>(2);

    /* Name field */
    auto nameGatherer = [this]
    { return m_targetActor ? m_targetActor->GetName() : "%undef%"; };
    auto nameProvider = [this](const std::string& p_newName)
    { if (m_targetActor) m_targetActor->SetName(p_newName); };
    //UI::GUIDrawer::DrawString(headerColumns, "Name", nameGatherer, nameProvider);

    /* Tag field */
    auto tagGatherer = [this]
    { return m_targetActor ? m_targetActor->GetTag() : "%undef%"; };
    auto tagProvider = [this](const std::string& p_newName)
    { if (m_targetActor) m_targetActor->SetTag(p_newName); };
    //UI::GUIDrawer::DrawString(headerColumns, "Tag", tagGatherer, tagProvider);

    /* Active field */
    auto activeGatherer = [this]
    { return m_targetActor ? m_targetActor->IsSelfActive() : false; };
    auto activeProvider = [this](bool p_active)
    { if (m_targetActor) m_targetActor->SetActive(p_active); };
    //UI::GUIDrawer::DrawBoolean(headerColumns, "Active", activeGatherer, activeProvider);

    /* Component select + button */
    {
        auto& componentSelectorWidget = m_inspectorHeader->CreateWidget<UI::Widgets::Selection::ComboBox>(0);
        componentSelectorWidget.lineBreak = false;
        componentSelectorWidget.choices.emplace(0, "Model Renderer");
        componentSelectorWidget.choices.emplace(1, "Camera");
        componentSelectorWidget.choices.emplace(2, "Light");
        componentSelectorWidget.choices.emplace(3, "MaterialRenderer");

        auto& addComponentButton = m_inspectorHeader->CreateWidget<UI::Widgets::Buttons::Button>("Add Component", Maths::Vector2{100.f, 0});
        addComponentButton.idleBackgroundColor = Maths::Color{0.7f, 0.5f, 0.f};
        addComponentButton.textColor = Maths::Color::White;
        addComponentButton.ClickedEvent += [&componentSelectorWidget, this]
        {
            using namespace NLS::Engine::Components;
            switch (componentSelectorWidget.currentChoice)
            {
                case 0:
                    GetTargetActor()->AddComponent<MeshRenderer>();
                    GetTargetActor()->AddComponent<MaterialRenderer>();
                    break;
                case 1:
                    GetTargetActor()->AddComponent<CameraComponent>();
                    break;
                case 2:
                    GetTargetActor()->AddComponent<LightComponent>();
                    break;
                case 3:
                    GetTargetActor()->AddComponent<MaterialRenderer>();
                    break;
            }

            componentSelectorWidget.ValueChangedEvent.Invoke(componentSelectorWidget.currentChoice);
        };

        componentSelectorWidget.ValueChangedEvent += [this, &addComponentButton](int p_value)
        {
            auto defineButtonsStates = [&addComponentButton](bool p_componentExists)
            {
                addComponentButton.disabled = p_componentExists;
                addComponentButton.idleBackgroundColor = !p_componentExists ? Maths::Color{0.7f, 0.5f, 0.f} : Maths::Color{0.1f, 0.1f, 0.1f};
            };
            using namespace NLS::Engine::Components;
            switch (p_value)
            {
                case 0:
                    defineButtonsStates(GetTargetActor()->GetComponent<MeshRenderer>());
                    return;
                case 1:
                    defineButtonsStates(GetTargetActor()->GetComponent<CameraComponent>());
                    return;
                case 2:
                    defineButtonsStates(GetTargetActor()->GetComponent<LightComponent>());
                    return;
                case 3:
                    defineButtonsStates(GetTargetActor()->GetComponent<MaterialRenderer>());
                    return;
            }
        };

        m_componentSelectorWidget = &componentSelectorWidget;
    }

    m_inspectorHeader->CreateWidget<UI::Widgets::Visual::Separator>();

    m_destroyedListener = Engine::GameObject::DestroyedEvent += [this](Engine::GameObject& p_destroyed)
    {
        if (&p_destroyed == m_targetActor)
            UnFocus();
    };
}
Inspector::~Inspector()
{
    Engine::GameObject::DestroyedEvent -= m_destroyedListener;

    UnFocus();
}

void Inspector::FocusActor(Engine::GameObject& p_target)
{
    if (m_targetActor)
        UnFocus();

    m_actorInfo->RemoveAllWidgets();

    m_targetActor = &p_target;

    m_componentAddedListener = m_targetActor->ComponentAddedEvent += [this](auto useless)
    { EDITOR_EXEC(DelayAction([this]
                              { Refresh(); })); };

    m_componentRemovedListener = m_targetActor->ComponentRemovedEvent += [this](auto useless)
    { EDITOR_EXEC(DelayAction([this]
                              { Refresh(); })); };

    m_inspectorHeader->enabled = true;

    CreateActorInspector(p_target);

    // Force component and script selectors to trigger their ChangedEvent to update button states
    m_componentSelectorWidget->ValueChangedEvent.Invoke(m_componentSelectorWidget->currentChoice);
    m_scriptSelectorWidget->ContentChangedEvent.Invoke(m_scriptSelectorWidget->content);

    EDITOR_EVENT(ActorSelectedEvent).Invoke(*m_targetActor);
}

void Inspector::UnFocus()
{
    if (m_targetActor)
    {
        m_targetActor->ComponentAddedEvent -= m_componentAddedListener;
        m_targetActor->ComponentRemovedEvent -= m_componentRemovedListener;
    }

    SoftUnFocus();
}

void Inspector::SoftUnFocus()
{
    if (m_targetActor)
    {
        EDITOR_EVENT(ActorUnselectedEvent).Invoke(*m_targetActor);
        m_inspectorHeader->enabled = false;
        m_targetActor = nullptr;
        m_actorInfo->RemoveAllWidgets();
    }
}

Engine::GameObject* Inspector::GetTargetActor() const
{
    return m_targetActor;
}

void Inspector::CreateActorInspector(Engine::GameObject& p_target)
{
    using namespace NLS::Engine::Components;
    std::map<std::string_view, UDRefl::SharedObject> components;

    for (auto component : p_target.GetComponents())
        if (component.GetType().GetName() != "TransformComponent")
            components[component.GetType().GetName()] = component;

    UDRefl::SharedObject transform = p_target.GetComponent(Type_of<TransformComponent>);
    if (transform)
        DrawComponent(*transform);

    for (auto& [name, instance] : components)
        DrawComponent(*instance);
}

void Inspector::DrawComponent(UDRefl::SharedObject p_component)
{
    using namespace NLS::Engine::Components;
    auto& header = m_actorInfo->CreateWidget<UI::Widgets::Layout::GroupCollapsable>("Component");
    header.closable = p_component.GetType() == Type_of<TransformComponent>;
    header.CloseEvent += [this, &header, &p_component]
    {
        if (p_component.Invoke<Engine::GameObject*>("gameobject")->RemoveComponent(p_component))
            m_componentSelectorWidget->ValueChangedEvent.Invoke(m_componentSelectorWidget->currentChoice);
    };
    auto& columns = header.CreateWidget<UI::Widgets::Layout::Columns>(2);
    columns.widths[0] = 200;
}

void Inspector::Refresh()
{
    if (m_targetActor)
    {
        m_actorInfo->RemoveAllWidgets();
        CreateActorInspector(*m_targetActor);
    }
}
} // namespace NLS::Editor::Panels
