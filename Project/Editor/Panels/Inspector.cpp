#include "Panels/Inspector.h"

#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Texts/Text.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Layout/GroupCollapsable.h>
#include <UI/GUIDrawer.h>
#include <ServiceLocator.h>
#include <ResourceManagement/ModelManager.h>
#include <ResourceManagement/TextureManager.h>
#include <ResourceManagement/ShaderManager.h>
#include <Reflection/Variant.h>
#include <imgui.h>
#include "Core/EditorActions.h"
#include "Components/TransformComponent.h"
#include "Panels/ComponentSearchPanel.h"
#include "Panels/ReflectedPropertyDrawer.h"
#include "GameObject.h"

namespace
{
using namespace NLS;

std::string GetComponentDisplayName(const NLS::Engine::Components::Component& component)
{
    std::string displayName = NLS::Editor::Panels::ComponentSearchPanel::MakeDisplayName(component.GetType());
    if (displayName.empty())
        displayName = "Component";

    return displayName;
}

void DrawComponentFallback(NLS::UI::Internal::WidgetContainer &root, Engine::Components::Component &component)
{
    root.CreateWidget<NLS::UI::Widgets::Text>("No reflected fields");
    root.CreateWidget<NLS::UI::Widgets::Text>(component.GetType().GetName());
}
} // namespace

namespace NLS::Editor::Panels
{
Inspector::Inspector(const std::string& p_title, bool p_opened, const NLS::UI::PanelWindowSettings& p_windowSettings)
    : NLS::UI::PanelWindow(p_title, p_opened, p_windowSettings)
{
    m_inspectorHeader = &CreateWidget<UI::Widgets::Group>();
    m_inspectorHeader->enabled = false;
    m_actorInfo = &CreateWidget<UI::Widgets::Group>();

    auto& headerColumns = m_inspectorHeader->CreateWidget<UI::Widgets::Columns>(2);
    headerColumns.widths[0] = 88;

    /* Name field */
    auto nameGatherer = [this]
    { return m_targetActor ? m_targetActor->GetName() : "%undef%"; };
    auto nameProvider = [this](const std::string& p_newName)
    { if (m_targetActor) m_targetActor->SetName(p_newName); };
    UI::GUIDrawer::DrawString(headerColumns, "Name", nameGatherer, nameProvider);

    /* Tag field */
    auto tagGatherer = [this]
    { return m_targetActor ? m_targetActor->GetTag() : "%undef%"; };
    auto tagProvider = [this](const std::string& p_newName)
    { if (m_targetActor) m_targetActor->SetTag(p_newName); };
    UI::GUIDrawer::DrawString(headerColumns, "Tag", tagGatherer, tagProvider);

    /* Active field */
    auto activeGatherer = [this]
    { return m_targetActor ? m_targetActor->IsSelfActive() : false; };
    auto activeProvider = [this](bool p_active)
    { if (m_targetActor) m_targetActor->SetActive(p_active); };
    UI::GUIDrawer::DrawBoolean(headerColumns, "Active", activeGatherer, activeProvider);

    auto& addComponentButton = m_inspectorHeader->CreateWidget<UI::Widgets::Button>("Add Component", Maths::Vector2{118.f, 0});
    addComponentButton.idleBackgroundColor = Maths::Color{0.23f, 0.49f, 0.82f};
    addComponentButton.hoveredBackgroundColor = Maths::Color{0.29f, 0.58f, 0.93f};
    addComponentButton.clickedBackgroundColor = Maths::Color{0.18f, 0.41f, 0.71f};
    addComponentButton.textColor = Maths::Color::White;
    addComponentButton.ClickedEvent += [this]
    {
        SyncComponentPicker();

        if (m_targetActor && m_componentPicker)
        {
            m_componentPicker->SetAnchorRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            m_componentPicker->OpenForActor(m_targetActor);
        }
    };

    m_inspectorHeader->CreateWidget<UI::Widgets::Separator>();
    m_inspectorHeader->CreateWidget<UI::Widgets::Spacing>(1);
    m_componentPicker = &m_inspectorHeader->CreateWidget<ComponentSearchPanel>();
    m_componentPicker->ComponentAddedEvent += [this]
    {
        Refresh();
    };

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
    SyncComponentPicker();

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
        if (m_componentPicker)
            m_componentPicker->ClearTarget();
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
    std::vector<Component*> components;

    for (const auto& component : p_target.GetComponents())
    {
        if (!component)
            continue;

        auto* rawComponent = component.get();
        if (dynamic_cast<TransformComponent*>(rawComponent))
            continue;

        components.push_back(rawComponent);
    }

    std::sort(
        components.begin(),
        components.end(),
        [](const Component* p_left, const Component* p_right)
        {
            if (p_left == nullptr || p_right == nullptr)
                return p_left < p_right;

            return GetComponentDisplayName(*p_left) < GetComponentDisplayName(*p_right);
        });

    if (auto* transform = p_target.GetComponent<TransformComponent>())
        DrawComponent(transform);

    for (auto* instance : components)
        DrawComponent(instance);
}

void Inspector::DrawComponent(Engine::Components::Component* p_component)
{
    using namespace NLS::Engine::Components;
    if (!p_component)
        return;

    const bool isTransform = dynamic_cast<TransformComponent*>(p_component) != nullptr;
    std::string title = GetComponentDisplayName(*p_component);
    if (title.empty())
        title = "Component";

    auto& header = m_actorInfo->CreateWidget<UI::Widgets::GroupCollapsable>(title);
    header.closable = !isTransform;
    header.opened = true;
    header.CloseEvent += [this, p_component]
    {
        if (!p_component)
            return;

        auto* owner = p_component->gameobject();
        if (!owner)
            return;

        const auto actorId = owner->GetWorldID();
        const auto componentType = p_component->GetType();
        EDITOR_EXEC(DelayAction([this, actorId, componentType]
        {
            auto* scene = EDITOR_CONTEXT(sceneManager).GetCurrentScene();
            if (!scene)
                return;

            auto* actor = scene->FindActorByID(actorId);
            if (!actor)
                return;

            auto* component = actor->GetComponent(componentType, true);
            if (!component)
                return;

            if (actor->RemoveComponent(component))
                SyncComponentPicker();
        }));
    };
    meta::Variant componentInstance(p_component, meta::variant_policy::WrapObject {});
    const auto componentType = p_component->GetType();
    const auto &fields = componentType.GetFields();
    if (fields.empty())
    {
        DrawComponentFallback(header, *p_component);
        m_actorInfo->CreateWidget<UI::Widgets::Spacing>(1);
        return;
    }

    ReflectedPropertyDrawerOptions options;
    options.labelWidth = 104.0f;
    DrawReflectedObject(header, componentInstance, options);

    m_actorInfo->CreateWidget<UI::Widgets::Spacing>(1);
}

void Inspector::Refresh()
{
    if (m_targetActor)
    {
        m_actorInfo->RemoveAllWidgets();
        CreateActorInspector(*m_targetActor);
        SyncComponentPicker();
    }
}

void Inspector::SyncComponentPicker()
{
    if (!m_componentPicker)
        return;

    if (m_targetActor)
    {
        if (m_componentPicker->GetTargetActor() != m_targetActor)
            m_componentPicker->SetTargetActor(m_targetActor);
        else
            m_componentPicker->NotifyActorComponentsChanged();
    }
    else
    {
        m_componentPicker->ClearTarget();
    }
}
} // namespace NLS::Editor::Panels
