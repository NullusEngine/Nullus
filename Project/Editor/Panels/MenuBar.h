#pragma once

#include <UI/Panels/PanelMenuBar.h>
#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Menu/MenuItem.h>
#include <UI/Widgets/Menu/MenuList.h>

#include <functional>

#include "Panels/ProjectSettings.h"
#include "Panels/ShortcutSettingsPanel.h"

namespace NLS::Editor::Panels
{
	class MenuBar : public UI::PanelMenuBar
	{
		using PanelMap = std::unordered_map<std::string, std::pair<std::reference_wrapper<UI::PanelWindow>, std::reference_wrapper<UI::Widgets::MenuItem>>>;

	public:
		/**
		* Constructor
		*/
		MenuBar();
		
		/**
		* Check inputs for menubar shortcuts
		* @param p_deltaTime
		*/
		void HandleShortcuts(float p_deltaTime);

        void DrawMenuEntries();
        void DrawDialogs();

		/**
		* Register a panel to the menu bar window menu
		*/
		void RegisterPanel(const std::string& p_name, UI::PanelWindow& p_panel);
        void RegisterProjectSettingsPanel(ProjectSettings& p_panel);
        void OpenProjectSettings();

        /**
        * Adds a command entry to a top-level menu. Only the existing top-level
        * menus and a lazily created "Tools" menu are accepted so scripted
        * editors cannot displace built-in editor chrome. Returns false when the
        * path is empty or the top-level menu is not allowed.
        * @param p_menuPath e.g. "Window/Asset Audit" or "Tools/Reimport"
        * @param p_execute
        */
        bool RegisterCommand(const std::string& p_menuPath, std::function<void()> p_execute);

		/**
		* @note This needs to be called after all other panels have been intialized, as the content of other
		* panels is required to initialize some settings
		*/
		void InitializeSettingsMenu();

	private:
		void CreateFileMenu();
        void CreateEditMenu();
		void CreateBuildMenu();
		void CreateWindowMenu();
		void createGameObjectsMenu();
		void CreateResourcesMenu();
		void CreateLayoutMenu();
		void CreateHelpMenu();

		void UpdateToggleableItems();
		void UpdateShortcutLabels();
		void OpenEveryWindows(bool p_state);

	private:
		PanelMap m_panels;
        // Top-level menus created for scripted editor commands. Entries are
        // owned by the widget tree; this map only caches the lookup.
        std::unordered_map<std::string, UI::Widgets::MenuList*> m_commandMenus;
        ShortcutSettingsPanel m_shortcutSettingsPanel;
        ProjectSettings* m_projectSettingsPanel = nullptr;
		UI::Widgets::MenuList* m_editMenu = nullptr;
		UI::Widgets::MenuList* m_settingsMenu = nullptr;
		UI::Widgets::MenuList* m_windowMenu = nullptr;
		UI::Widgets::MenuItem* m_newSceneItem = nullptr;
		UI::Widgets::MenuItem* m_saveSceneItem = nullptr;
		UI::Widgets::MenuItem* m_saveSceneAsItem = nullptr;
		UI::Widgets::MenuItem* m_undoItem = nullptr;
		UI::Widgets::MenuItem* m_redoItem = nullptr;
		UI::Widgets::MenuItem* m_renderDocCaptureItem = nullptr;
		UI::Widgets::MenuItem* m_renderDocOpenLatestItem = nullptr;
	};
}
