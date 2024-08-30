#include <UI/Widgets/Menu/MenuList.h>
#include <UI/Widgets/Menu/MenuItem.h>

#include "Core/EditorActions.h"
#include "Utils/ActorCreationMenu.h"
#include "GameObject.h"
#include "Components/LightComponent.h"
#include "Components/CameraComponent.h"
using namespace NLS;
std::function<void()> Combine(std::function<void()> p_a, std::optional<std::function<void()>> p_b)
{
    if (p_b.has_value())
    {
        return [=]()
        {
            p_a();
            p_b.value()();
        };
    }

    return p_a;
}

template<class T>
std::function<void()> ActorWithComponentCreationHandler(Engine::GameObject* p_parent, std::optional<std::function<void()>> p_onItemClicked)
{
    return Combine(EDITOR_BIND(CreateMonoComponentActor<T>, true, p_parent), p_onItemClicked);
}

std::function<void()> ActorWithModelComponentCreationHandler(Engine::GameObject* p_parent, const std::string& p_modelName, std::optional<std::function<void()>> p_onItemClicked)
{
    return Combine(EDITOR_BIND(CreateActorWithModel, ":Models\\" + p_modelName + ".fbx", true, p_parent, p_modelName), p_onItemClicked);
}

void Editor::Utils::ActorCreationMenu::GenerateActorCreationMenu(UI::Widgets::MenuList& p_menuList, Engine::GameObject* p_parent, std::optional<std::function<void()>> p_onItemClicked)
{
    using namespace NLS::UI;
    using namespace NLS::Engine::Components;

    p_menuList.CreateWidget<Widgets::MenuItem>("Create Empty").ClickedEvent += Combine(EDITOR_BIND(CreateEmptyActor, true, p_parent, ""), p_onItemClicked);

    auto& primitives = p_menuList.CreateWidget<Widgets::MenuList>("Primitives");
    auto& physicals = p_menuList.CreateWidget<Widgets::MenuList>("Physicals");
    auto& lights = p_menuList.CreateWidget<Widgets::MenuList>("Lights");
    auto& audio = p_menuList.CreateWidget<Widgets::MenuList>("Audio");
    auto& others = p_menuList.CreateWidget<Widgets::MenuList>("Others");

    primitives.CreateWidget<Widgets::MenuItem>("Cube").ClickedEvent              += ActorWithModelComponentCreationHandler(p_parent, "Cube", p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Sphere").ClickedEvent            += ActorWithModelComponentCreationHandler(p_parent, "Sphere", p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Cone").ClickedEvent              += ActorWithModelComponentCreationHandler(p_parent, "Cone", p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Cylinder").ClickedEvent          += ActorWithModelComponentCreationHandler(p_parent, "Cylinder", p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Plane").ClickedEvent             += ActorWithModelComponentCreationHandler(p_parent, "Plane", p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Gear").ClickedEvent              += ActorWithModelComponentCreationHandler(p_parent, "Gear", p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Helix").ClickedEvent             += ActorWithModelComponentCreationHandler(p_parent, "Helix", p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Pipe").ClickedEvent              += ActorWithModelComponentCreationHandler(p_parent, "Pipe", p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Pyramid").ClickedEvent           += ActorWithModelComponentCreationHandler(p_parent, "Pyramid", p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Torus").ClickedEvent             += ActorWithModelComponentCreationHandler(p_parent, "Torus", p_onItemClicked);
    lights.CreateWidget<Widgets::MenuItem>("Light").ClickedEvent                 += ActorWithComponentCreationHandler<LightComponent>(p_parent, p_onItemClicked);
    others.CreateWidget<Widgets::MenuItem>("Camera").ClickedEvent                += ActorWithComponentCreationHandler<CameraComponent>(p_parent, p_onItemClicked);
}
