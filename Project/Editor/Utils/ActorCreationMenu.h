#pragma once

#include <functional>

namespace NLS::UI::Widgets::Menu
{
    class MenuList;
}

namespace NLS::Engine
{
    class GameObject;
}

namespace NLS::Editor::Utils
{
    /**
    * Class exposing tools to generate an actor creation menu
    */
    class ActorCreationMenu
    {
    public:
        /**
        * Disabled constructor
        */
        ActorCreationMenu() = delete;

        /**
        * Generates an actor creation menu under the given MenuList item.
        * Also handles custom additionnal OnClick callback
        * @param p_menuList
        * @param p_parent
        * @param p_onItemClicked
        */
        static void GenerateActorCreationMenu(NLS::UI::Widgets::Menu::MenuList& p_menuList, Engine::GameObject* p_parent = nullptr, std::optional<std::function<void()>> p_onItemClicked = {});
    };
}
