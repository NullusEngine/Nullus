#pragma once

#include <variant>

#include <Filesystem/IniFile.h>
#include <Eventing/Event.h>

#include <UI/Panels/PanelWindow.h>

namespace NLS::Render::Resources
{
	class Material;
	class Model;
	class Texture2D;
}

namespace NLS::UI::Widgets
{
	class AWidget;
	class Button;
	class Columns;
	class Group;
	class Text;
}

namespace NLS::Editor::Panels
{
	class AssetProperties : public UI::PanelWindow
	{
	public:
		using EditableAssets = std::variant<Render::Resources::Model*, Render::Resources::Material*, Render::Resources::Texture2D*>;

		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		AssetProperties(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);

		/**
		* Defines the target of the asset settings editor
		* @param p_path
		*/
		void SetTarget(const std::string& p_path);

        /**
        * Refresh the panel to show the current target settings
        */
        void Refresh();

		/**
		* Launch the preview of the target asset
		*/
		void Preview();

	private:
		void CreateHeaderButtons();
        void CreateAssetSelector();
		void CreateSettings();
		void CreateInfo();
		void CreateModelSettings();
		void CreateTextureSettings();
		void Apply();

	private:
		std::string m_resource;

        Event<> m_targetChanged;
        UI::Widgets::Group* m_settings = nullptr;
        UI::Widgets::Group* m_info = nullptr;
        UI::Widgets::Button* m_applyButton = nullptr;
        UI::Widgets::Button* m_revertButton = nullptr;
        UI::Widgets::Button* m_previewButton = nullptr;
        UI::Widgets::Button* m_resetButton = nullptr;
        UI::Widgets::AWidget* m_headerSeparator = nullptr;
        UI::Widgets::AWidget* m_headerLineBreak = nullptr;
		UI::Widgets::Columns* m_settingsColumns = nullptr;
		UI::Widgets::Columns* m_infoColumns = nullptr;
        UI::Widgets::Text* m_assetSelector = nullptr;
		std::unique_ptr<Filesystem::IniFile> m_metadata;
	};
}
