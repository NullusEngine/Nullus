#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <Filesystem/IniFile.h>
#include <Eventing/Event.h>

#include <UI/Panels/PanelWindow.h>

#include "Assets/AssetImporterSettings.h"
#include "Assets/ModelTextureResolutionReport.h"

namespace NLS::Render::Resources
{
	class Material;
	class Mesh;
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
    struct ModelTextureAssetPropertiesRow
    {
        std::string sourceStableKey;
        std::string resolutionKind;
        std::string targetAssetId;
        std::string targetSubAssetKey;
        std::string targetEditorPath;
        std::vector<std::string> diagnosticCodes;
        bool hasWarnings = false;
        bool hasInvalidTargetWarning = false;
        bool hasUnsupportedEncodingWarning = false;
        bool usesOrderDerivedStableKey = false;
    };

    struct ModelTextureAssetPropertiesView
    {
        NLS::Editor::Assets::ModelTextureResolutionSettings settings;
        bool hasCurrentReport = false;
        bool reportMalformed = false;
        bool reportStale = false;
        bool reimportRequired = false;
        std::vector<ModelTextureAssetPropertiesRow> rows;
    };

    ModelTextureAssetPropertiesView BuildModelTextureAssetPropertiesView(
        const NLS::Core::Assets::AssetMeta& modelMeta,
        const std::string& targetPlatform,
        const std::optional<std::string>& reportText);
    void StoreModelTextureAssetPropertiesSettings(
        NLS::Core::Assets::AssetMeta& modelMeta,
        const NLS::Editor::Assets::ModelTextureResolutionSettings& settings);
    void StoreModelTextureAssetPropertiesRemap(
        NLS::Core::Assets::AssetMeta& modelMeta,
        const std::string& stableSourceKey,
        const NLS::Core::Assets::AssetId& targetAssetId,
        const std::string& targetSubAssetKey,
        const std::string& targetEditorPath);
    void ClearModelTextureAssetPropertiesRemap(
        NLS::Core::Assets::AssetMeta& modelMeta,
        const std::string& stableSourceKey);

	class AssetProperties : public UI::PanelWindow
	{
	public:
		using EditableAssets = std::variant<Render::Resources::Mesh*, Render::Resources::Material*, Render::Resources::Texture2D*>;

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
            void CreateModelTextureResolutionProperties();
			void CreateTextureSettings();
		void Apply();
		void Reimport();

	private:
		std::string m_resource;

        Event<> m_targetChanged;
        UI::Widgets::Group* m_settings = nullptr;
        UI::Widgets::Group* m_info = nullptr;
        UI::Widgets::Button* m_applyButton = nullptr;
        UI::Widgets::Button* m_reimportButton = nullptr;
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
