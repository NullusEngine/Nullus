#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <queue>

#include <Core/AssetFileWatcher.h>
#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Layout/TreeNode.h>
#include <Rendering/Resources/Loaders/TextureLoader.h>
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetThumbnailService.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/EditorStartupAssetPreimport.h"
struct ImVec2;
namespace NLS::Render::Resources
{
class Texture2D;
}
namespace NLS::Render::RHI
{
class RHITextureView;
}
namespace NLS::Engine
{
class GameObject;
}
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
		~AssetBrowser();

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
		enum class ProjectBrowserTextDialogKind
		{
			None,
			RenameFolder,
			RenameFile,
			CreateFolder,
			CreateScene,
			CreateStandardShader,
			CreateStandardPBRShader,
			CreateUnlitShader,
			CreateLambertShader,
			CreateEmptyMaterial,
			CreateStandardMaterial,
			CreateStandardPBRMaterial,
			CreateUnlitMaterial,
			CreateLambertMaterial
		};

		struct ProjectBrowserTextDialogState
		{
			ProjectBrowserTextDialogKind kind = ProjectBrowserTextDialogKind::None;
			std::string title;
			std::filesystem::path targetAbsoluteFolder;
			std::string targetProjectRelativeFolder;
			std::filesystem::path sourceAbsolutePath;
			std::array<char, 256u> buffer {};
			bool requestOpen = false;
		};

		struct ThumbnailTextureDecodeResult;

		void OnBeforeDrawWidgets() override;
		void OnAfterDrawWidgets() override;
		void StartWatchersAsync();
		void StartWatchersSynchronously();
		void CompleteWatcherStartupIfReady();
		void ConsumeWatcherChangesAndSchedulePreimport();
		void RequestRefresh();
		void ScheduleProjectAssetPreimport(NLS::Editor::Assets::AssetPreimportRequest request);
		void RefreshPreservingExpandedFolders();
		void RebuildProjectFolderTreePresentation();
		void RebuildProjectAssetPresentation(NLS::Editor::Assets::AssetBrowserRefreshPlan refreshPlan);
		void SelectProjectFolder(const std::string& projectRelativePath);
		void DrawProjectAssetBrowser();
		void DrawProjectFolderTree(const NLS::Editor::Assets::AssetBrowserFolderNode& node);
		void DrawProjectBreadcrumb();
		void DrawProjectFilterBar();
		void DrawCurrentFolderGrid();
		void RequestProjectBrowserTextDialog(
			ProjectBrowserTextDialogKind kind,
			std::string title,
			const std::filesystem::path& targetAbsoluteFolder,
			std::string targetProjectRelativeFolder,
			const std::filesystem::path& sourceAbsolutePath,
			std::string defaultName);
		void DrawProjectBrowserTextDialog();
		bool CommitProjectBrowserTextDialog();
		void DrawProjectFolderContextMenu(
			const std::string& popupId,
			const std::string& projectRelativeFolder,
			const std::filesystem::path& absoluteFolder);
		void DrawProjectGridItemContextMenu(const NLS::Editor::Assets::AssetBrowserItem& item);
		void DrawProjectGridItemThumbnail(
			const NLS::Editor::Assets::AssetBrowserItem& item,
			const ImVec2& iconMin,
			const ImVec2& iconMax,
			float thumbnailSize,
			bool hovered);
		void DrawProjectGridItemDragSource(const NLS::Editor::Assets::AssetBrowserItem& item);
		void DrawProjectFolderDropTarget(
			const std::string& projectRelativeFolder,
			const std::filesystem::path& absoluteFolder);
		void SelectProjectGridItem(const NLS::Editor::Assets::AssetBrowserItem& item);
		void OpenProjectGridItem(const NLS::Editor::Assets::AssetBrowserItem& item);
		void OpenProjectGridItemProperties(const NLS::Editor::Assets::AssetBrowserItem& item);
		void PreviewProjectGridItem(const NLS::Editor::Assets::AssetBrowserItem& item);
		void RebuildProjectAssetPresentationAfterWorkflow();
		void ScheduleProjectAssetPreimportForPath(const std::filesystem::path& projectRelativePath);
		bool MoveOrCopyProjectBrowserFolderIntoFolder(
			const std::string& receivedProjectRelativeFolder,
			const std::filesystem::path& targetAbsoluteFolder);
		bool MoveOrCopyProjectBrowserFileIntoFolder(
			const std::string& receivedResourcePath,
			const std::filesystem::path& targetAbsoluteFolder);
		bool SaveHierarchyObjectAsPrefabIntoFolder(
			Engine::GameObject* gameObject,
			const std::string& targetProjectRelativeFolder,
			const std::filesystem::path& targetAbsoluteFolder);
		void* ResolveCachedThumbnailTextureHandle(const std::filesystem::path& imagePath);
		bool LoadCachedThumbnailTexture(const std::string& normalizedPath);
		bool LoadDecodedCachedThumbnailTexture(ThumbnailTextureDecodeResult result);
		static ThumbnailTextureDecodeResult DecodeCachedThumbnailTexture(std::string normalizedPath);
		void QueueCachedThumbnailTextureLoad(const std::filesystem::path& imagePath);
		void PumpQueuedCachedThumbnailTextureLoads();
		void StartQueuedCachedThumbnailTextureDecodes();
		void ConsumeCompletedCachedThumbnailTextureDecodes();
		void PumpRetiredProjectAssetDatabaseRefreshes();
		void DiscardProjectAssetDatabaseRefresh();
		void PumpObjectReferencePickerEntriesRefresh();
		void RequestObjectReferencePickerEntriesRefresh();
		void DiscardObjectReferencePickerEntriesRefresh();
		void DestroyCachedThumbnailTextures(bool force);
		void ReleaseCachedThumbnailTexture(const std::string& normalizedPath);
		void PruneCachedThumbnailTextures();
		void UpdateThumbnailGenerationScope();
		void ParseFolder(UI::Widgets::TreeNode& p_root, const std::filesystem::directory_entry& p_directory, bool p_isEngineItem, bool p_scriptFolder = false);
		void ConsiderItem(UI::Widgets::TreeNode* p_root, const std::filesystem::directory_entry& p_entry, bool p_isEngineItem, bool p_autoOpen = false, bool p_scriptFolder = false);

	public:
		static const std::string __FILENAMES_CHARS;

	private:
		std::string m_engineAssetFolder;
		std::string m_projectAssetFolder;
		UI::Widgets::Group* m_assetList;
		NLS::UI::Widgets::TextClickable* m_selectedAsset = nullptr;
		std::string m_selectedProjectFolder = "Assets";
		std::string m_selectedProjectItem;
		std::string m_projectSearchQuery;
		NLS::Editor::Assets::AssetBrowserItemType m_projectTypeFilter = NLS::Editor::Assets::AssetBrowserItemType::All;
		float m_thumbnailSize = 96.0f;
		NLS::Editor::Assets::AssetBrowserFolderNode m_projectFolderTree;
		std::vector<NLS::Editor::Assets::AssetBrowserItem> m_unfilteredCurrentFolderItems;
		std::vector<NLS::Editor::Assets::AssetBrowserItem> m_currentFolderItems;
		std::vector<NLS::Editor::Assets::AssetBrowserBreadcrumbSegment> m_currentBreadcrumb;
		NLS::Editor::Assets::AssetThumbnailService m_thumbnailService;
		std::vector<NLS::Editor::Assets::AssetBrowserItem> m_visibleThumbnailItems;
		bool m_visibleThumbnailItemsKnown = false;
		std::unordered_map<std::string, NLS::Editor::Assets::AssetThumbnailServiceResult> m_thumbnailResultsByItemKey;
		std::unordered_map<std::string, std::string> m_thumbnailItemKeyByCacheKey;
		struct ThumbnailTextureCacheEntry
		{
			NLS::Render::Resources::Texture2D* texture = nullptr;
			std::shared_ptr<NLS::Render::RHI::RHITextureView> textureView;
		};
		struct ThumbnailTextureDecodeResult
		{
			std::string normalizedPath;
			std::vector<uint8_t> rgbaPixels;
			uint32_t width = 0u;
			uint32_t height = 0u;
		};
		struct InFlightThumbnailTextureDecode
		{
			std::string normalizedPath;
			std::future<ThumbnailTextureDecodeResult> future;
		};
		struct ObjectReferencePickerRefresh
		{
			uint64_t generation = 0u;
			std::future<std::vector<NLS::Editor::Assets::ObjectReferencePickerEntry>> future;
		};

		struct AssetDatabaseRefresh
		{
			std::filesystem::path root;
			std::future<std::unique_ptr<NLS::Editor::Assets::AssetDatabaseFacade>> future;
		};
		std::unordered_map<std::string, ThumbnailTextureCacheEntry> m_thumbnailTexturesByPath;
		std::vector<std::string> m_thumbnailTextureLru;
		std::unordered_set<std::string> m_thumbnailTexturesUsedThisFrame;
		std::unordered_set<std::string> m_thumbnailTexturesPendingRelease;
		std::vector<std::string> m_thumbnailTextureLoadQueue;
		std::unordered_set<std::string> m_thumbnailTexturesQueuedForLoad;
		std::unordered_set<std::string> m_thumbnailTexturesFailedToLoad;
		std::vector<InFlightThumbnailTextureDecode> m_thumbnailTextureDecodes;
		std::unordered_set<std::string> m_thumbnailTexturesDecoding;
		std::optional<ObjectReferencePickerRefresh> m_objectReferencePickerRefresh;
		std::vector<ObjectReferencePickerRefresh> m_retiredObjectReferencePickerRefreshes;
		uint64_t m_objectReferencePickerRefreshGeneration = 0u;
		bool m_objectReferencePickerRefreshRequested = false;
		uint32_t m_lastThumbnailRequestSize = 0u;
		std::string m_lastThumbnailGenerationScopeKey;
		bool m_thumbnailGenerationScopeDirty = true;
		std::unique_ptr<NLS::Editor::Assets::AssetDatabaseFacade> m_projectAssetDatabase;
		std::filesystem::path m_projectAssetDatabaseRoot;
		bool m_projectAssetDatabaseReady = false;
		std::optional<AssetDatabaseRefresh> m_projectAssetDatabaseRefresh;
		std::vector<AssetDatabaseRefresh> m_retiredProjectAssetDatabaseRefreshes;
		bool m_projectAssetDatabaseRefreshQueuedAfterInFlight = false;
		ProjectBrowserTextDialogState m_projectBrowserTextDialog;
		std::pair<std::string, UI::Widgets::Group*> m_projectGridDragPairPayload;
		std::unordered_map<UI::Widgets::TreeNode*, std::string> m_pathUpdate;
		std::unordered_set<std::string> m_expandedFolders;
		std::unordered_set<std::string> m_expandedProjectFolders;
		Core::AssetFileWatcher m_engineAssetsWatcher;
		Core::AssetFileWatcher m_projectAssetsWatcher;
		std::future<void> m_watcherStartup;
		bool m_watchersStartupQueued = false;
		bool m_watchersReadyRefreshQueued = false;
		bool m_refreshRequested = false;
		bool m_projectFolderTreeRefreshRequested = false;
		bool m_startupWatcherPreimportGateOpen = true;
		bool m_projectAssetPreimportRunning = false;
		std::optional<NLS::Editor::Assets::AssetPreimportRequest> m_pendingProjectAssetPreimportRequest;
	};
}
