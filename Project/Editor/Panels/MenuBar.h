#pragma once

#include <UI/Panels/PanelMenuBar.h>
#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Menu/MenuItem.h>

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

		/**
		* Register a panel to the menu bar window menu
		*/
		void RegisterPanel(const std::string& p_name, UI::PanelWindow& p_panel);

		/**
		* @note This needs to be called after all other panels have been intialized, as the content of other
		* panels is required to initialize some settings
		*/
		void InitializeSettingsMenu();

	private:
		void CreateFileMenu();
		void CreateBuildMenu();
		void CreateWindowMenu();
		void CreateActorsMenu();
		void CreateResourcesMenu();
		void CreateSettingsMenu();
		void CreateLayoutMenu();
		void CreateHelpMenu();

		void UpdateToggleableItems();
		void OpenEveryWindows(bool p_state);

	private:
		PanelMap m_panels;
		UI::Widgets::MenuList* m_settingsMenu = nullptr;
		UI::Widgets::MenuList* m_windowMenu = nullptr;
	};
}