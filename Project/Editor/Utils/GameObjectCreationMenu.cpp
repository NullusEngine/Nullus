#include <UI/Widgets/Menu/MenuList.h>
#include <UI/Widgets/Menu/MenuItem.h>

#include "Core/EditorActions.h"
#include "Utils/GameObjectCreationMenu.h"
#include "GameObject.h"
#include "Components/LightComponent.h"
#include "Components/CameraComponent.h"
#include "Components/MeshFilter.h"
#include "Engine/PrimitiveType.h"
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
std::function<void()> GameObjectWithComponentCreationHandler(Engine::GameObject* p_parent, std::optional<std::function<void()>> p_onItemClicked)
{
    return Combine(EDITOR_BIND(CreateMonoComponentGameObject<T>, true, p_parent), p_onItemClicked);
}

std::function<void()> PrimitiveCreationHandler(Engine::GameObject* p_parent, Engine::PrimitiveType p_type, std::optional<std::function<void()>> p_onItemClicked)
{
    return Combine(EDITOR_BIND(CreatePrimitive, p_type, true, p_parent), p_onItemClicked);
}

void Editor::Utils::GameObjectCreationMenu::GenerateGameObjectCreationMenu(UI::Widgets::MenuList& p_menuList, Engine::GameObject* p_parent, std::optional<std::function<void()>> p_onItemClicked)
{
    using namespace NLS::UI;
    using namespace NLS::Engine::Components;

    p_menuList.CreateWidget<Widgets::MenuItem>("Create Empty").ClickedEvent += Combine(EDITOR_BIND(CreateEmptyGameObject, true, p_parent, ""), p_onItemClicked);

    auto& primitives = p_menuList.CreateWidget<Widgets::MenuList>("Primitives");
    auto& physicals = p_menuList.CreateWidget<Widgets::MenuList>("Physicals");
    auto& lights = p_menuList.CreateWidget<Widgets::MenuList>("Lights");
    auto& audio = p_menuList.CreateWidget<Widgets::MenuList>("Audio");
    auto& others = p_menuList.CreateWidget<Widgets::MenuList>("Others");

    primitives.CreateWidget<Widgets::MenuItem>("Cube").ClickedEvent              += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Cube, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Sphere").ClickedEvent            += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Sphere, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Cone").ClickedEvent              += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Cone, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Cylinder").ClickedEvent          += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Cylinder, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Plane").ClickedEvent             += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Plane, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Gear").ClickedEvent              += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Gear, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Helix").ClickedEvent             += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Helix, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Pipe").ClickedEvent              += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Pipe, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Pyramid").ClickedEvent           += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Pyramid, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Torus").ClickedEvent             += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Torus, p_onItemClicked);
    lights.CreateWidget<Widgets::MenuItem>("Light").ClickedEvent                 += GameObjectWithComponentCreationHandler<LightComponent>(p_parent, p_onItemClicked);
    others.CreateWidget<Widgets::MenuItem>("Camera").ClickedEvent                += GameObjectWithComponentCreationHandler<CameraComponent>(p_parent, p_onItemClicked);
}
