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
    * Class exposing tools to generate an GameObject creation menu
    */
    class GameObjectCreationMenu
    {
    public:
        /**
        * Disabled constructor
        */
        GameObjectCreationMenu() = delete;

        /**
        * Generates an GameObject creation menu under the given MenuList item.
        * Also handles custom additionnal OnClick callback
        * @param p_menuList
        * @param p_parent
        * @param p_onItemClicked
        */
        static void GenerateGameObjectCreationMenu(NLS::UI::Widgets::MenuList& p_menuList, Engine::GameObject* p_parent = nullptr, std::optional<std::function<void()>> p_onItemClicked = {});
    };
}
