#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <future>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <queue>

#include <Core/AssetFileWatcher.h>
#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Layout/TreeNode.h>
#include <Rendering/Resources/Loaders/TextureLoader.h>
#include "Assets/EditorAssetDatabase.h"
#include "Assets/EditorStartupAssetPreimport.h"
namespace UI::Widgets::Layout
{
class Group;
}
namespace NLS::UI::Widgets
{
class TextClickable;
}
namespace NLS::Editor::Panels
{
	/**
	* A panel that handle asset management
	*/
	class AssetBrowser : public UI::PanelWindow
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @parma p_opened
		* @param p_windowSettings
		* @param p_engineAssetFolder
		* @param p_projectAssetFolder
		* @param p_projectScriptFolder
		*/
		AssetBrowser(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings,
			const std::string& p_engineAssetFolder = "",
			const std::string& p_projectAssetFolder = "",
			const std::string& p_projectScriptFolder = ""
		);

		/**
		* Fill the asset browser panels with widgets corresponding to elements in the asset folder
		*/
		void Fill();

		/**
		* Clear the asset browser widgets
		*/
		void Clear();

		/**
		* Refresh the asset browser widgets (Clear + Fill)
		*/
		void Refresh();
		void PrepareStartupWatchers();
		void AdoptStartupWatchers(
			Core::AssetFileWatcher engineAssetsWatcher,
			Core::AssetFileWatcher projectAssetsWatcher);
		bool RunStartupWatcherPreimport(
			const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink = {});
		bool CompleteStartupWatcherPreimportGate(
			const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink = {});

	private:
		void OnBeforeDrawWidgets() override;
		void StartWatchersAsync();
		void StartWatchersSynchronously();
		void CompleteWatcherStartupIfReady();
		void ConsumeWatcherChangesAndSchedulePreimport();
		void RequestRefresh();
		void ScheduleProjectAssetPreimport(NLS::Editor::Assets::AssetPreimportRequest request);
		void RefreshPreservingExpandedFolders();
		void ParseFolder(UI::Widgets::TreeNode& p_root, const std::filesystem::directory_entry& p_directory, bool p_isEngineItem, bool p_scriptFolder = false);
		void ConsiderItem(UI::Widgets::TreeNode* p_root, const std::filesystem::directory_entry& p_entry, bool p_isEngineItem, bool p_autoOpen = false, bool p_scriptFolder = false);

	public:
		static const std::string __FILENAMES_CHARS;

	private:
		std::string m_engineAssetFolder;
		std::string m_projectAssetFolder;
		UI::Widgets::Group* m_assetList;
		NLS::UI::Widgets::TextClickable* m_selectedAsset = nullptr;
		std::unordered_map<UI::Widgets::TreeNode*, std::string> m_pathUpdate;
		std::unordered_set<std::string> m_expandedFolders;
		Core::AssetFileWatcher m_engineAssetsWatcher;
		Core::AssetFileWatcher m_projectAssetsWatcher;
		std::future<void> m_watcherStartup;
		bool m_watchersStartupQueued = false;
		bool m_watchersReadyRefreshQueued = false;
		bool m_refreshRequested = false;
		bool m_startupWatcherPreimportGateOpen = true;
		bool m_projectAssetPreimportRunning = false;
		std::optional<NLS::Editor::Assets::AssetPreimportRequest> m_pendingProjectAssetPreimportRequest;
	};
}
