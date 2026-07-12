#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <vector>

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
enum class AssetBrowserThumbnailDrawOutcome : uint8_t
{
    Thumbnail,
    Fallback,
    TypeFallback
};

struct AssetBrowserThumbnailDrawOutcomePathTotal
{
    std::string path;
    size_t count = 0u;
};

struct AssetBrowserThumbnailDrawOutcomeTelemetrySnapshot
{
    size_t thumbnailDrawCount = 0u;
    size_t fallbackDrawCount = 0u;
    size_t typeFallbackDrawCount = 0u;
    size_t droppedPathCount = 0u;
    std::vector<AssetBrowserThumbnailDrawOutcomePathTotal> pathTotals;
};

void RecordAssetBrowserThumbnailDrawOutcomeTelemetry(
    std::string_view assetPath,
    AssetBrowserThumbnailDrawOutcome outcome);
AssetBrowserThumbnailDrawOutcomeTelemetrySnapshot SnapshotAssetBrowserThumbnailDrawOutcomeTelemetry();

#if defined(NLS_ENABLE_TEST_HOOKS)
void ClearAssetBrowserThumbnailDrawOutcomeTelemetryForTesting();
#endif

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
        const std::string& p_projectScriptFolder = "");
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
    NLS::Editor::Assets::StartupWatcherPreimportResult RunStartupWatcherPreimport(
        const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink = {});
	    NLS::Editor::Assets::StartupWatcherPreimportResult CompleteStartupWatcherPreimportGate(
	        const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink = {});
	    void SelectProjectFolderForValidation(const std::string& projectRelativePath);

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
        CreateUnlitTextureShader,
        CreateEmptyMaterial,
        CreateStandardMaterial,
        CreateStandardPBRMaterial,
        CreateUnlitMaterial,
        CreateDefaultSurfaceMaterial
    };

    struct ProjectBrowserTextDialogState
    {
        ProjectBrowserTextDialogKind kind = ProjectBrowserTextDialogKind::None;
        std::string title;
        std::filesystem::path targetAbsoluteFolder;
        std::string targetProjectRelativeFolder;
        std::filesystem::path sourceAbsolutePath;
        std::array<char, 256u> buffer{};
        bool requestOpen = false;
    };

    struct ProjectBrowserInlineRenameState
    {
        bool active = false;
        bool focusRequested = false;
        bool pending = false;
        double pendingSince = 0.0;
        NLS::Editor::Assets::AssetBrowserItemKind kind = NLS::Editor::Assets::AssetBrowserItemKind::SourceAsset;
        std::string sourceProjectRelativePath;
        std::filesystem::path sourceAbsolutePath;

        std::filesystem::path targetAbsoluteFolder;
        std::string targetProjectRelativeFolder;
        std::array<char, 256u> buffer{};
    };

	    struct ThumbnailTextureDecodeResult;
	    struct AssetDatabaseRefreshResult;
	    struct AssetDatabaseRetirementState;

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
    bool DrawProjectFolderTree(const NLS::Editor::Assets::AssetBrowserFolderNode& node);
    bool IsAssetBrowserInteractive() const;
    void MarkProjectAssetDisplayItemsDirty();
    bool ApplyProjectAssetDisclosureImmediately(
        const NLS::Editor::Assets::AssetBrowserItem& sourceItem);
    void RebuildProjectAssetDisplayItemsIfNeeded();
    void SetVisibleThumbnailItems(std::vector<NLS::Editor::Assets::AssetBrowserItem> visibleItems);
    void DrawProjectBreadcrumb();
    void DrawProjectFilterBar();
    void DrawCurrentFolderGrid();
    void DrawCurrentFolderList(
        const std::vector<NLS::Editor::Assets::AssetBrowserDisplayItem>& displayItems);
    void DrawAssetBrowserFooter();
    void RequestProjectBrowserTextDialog(
        ProjectBrowserTextDialogKind kind,
        std::string title,
        const std::filesystem::path& targetAbsoluteFolder,
        std::string targetProjectRelativeFolder,
        const std::filesystem::path& sourceAbsolutePath,
        std::string defaultName);
    void DrawProjectBrowserTextDialog();
    bool CommitProjectBrowserTextDialog();
    void DrawProjectCurrentFolderContextMenu(
        const std::string& popupId,
        const std::string& projectRelativeFolder,
        const std::filesystem::path& absoluteFolder);
    void DrawProjectFolderContextMenu(
        const std::string& popupId,
        const std::string& projectRelativeFolder,
        const std::filesystem::path& absoluteFolder);
    void DrawProjectGridItemContextMenu(const NLS::Editor::Assets::AssetBrowserItem& item);
    void DrawProjectGridItemInlineRename(
        const NLS::Editor::Assets::AssetBrowserItem& item,
        const ImVec2& labelMin,
        const ImVec2& labelMax);
    void DrawProjectGridItemThumbnail(
        const NLS::Editor::Assets::AssetBrowserItem& item,
        const ImVec2& iconMin,
        const ImVec2& iconMax,
        float thumbnailSize,
        bool hovered,
        bool compact = false);
    void DrawProjectGridItemDragSource(const NLS::Editor::Assets::AssetBrowserItem& item);
    void SchedulePrefabHotCachePreloadForHoveredItem(
        const NLS::Editor::Assets::AssetBrowserItem& item,
        bool hovered);
    void SchedulePrefabHotCachePreloadForVisibleItems(
        const std::vector<NLS::Editor::Assets::AssetBrowserItem>& visibleItems);
    void FlushPendingVisiblePrefabHotCachePreload();
    void PumpStandardPbrShaderPassPrewarm();
    void PumpThumbnailPreviewRenderWarmup();
    void SchedulePrefabHotCachePreloadForDragPayload(
        const NLS::Editor::Assets::EditorAssetDragPayload& payload);
    void DrawProjectFolderDropTarget(
        const std::string& projectRelativeFolder,
        const std::filesystem::path& absoluteFolder);
    void SelectProjectGridItem(const NLS::Editor::Assets::AssetBrowserItem& item);
    void OpenProjectGridItem(const NLS::Editor::Assets::AssetBrowserItem& item);
    void OpenProjectGridItemProperties(const NLS::Editor::Assets::AssetBrowserItem& item);
    void PreviewProjectGridItem(const NLS::Editor::Assets::AssetBrowserItem& item);
    void RebuildProjectAssetPresentationAfterWorkflow();
    void ScheduleProjectAssetPreimportForPath(std::string projectRelativePath);
    void ScheduleProjectAssetPreimportForPath(const std::filesystem::path& projectRelativePath);
    void HandleProjectAssetBrowserShortcuts();
    void HandleProjectAssetBrowserDroppedFiles(const std::vector<std::string>& paths);
    void BeginInlineRenameProjectItem(const NLS::Editor::Assets::AssetBrowserItem& item);
    void CancelInlineRenameProjectItem();
    bool CommitInlineRenameProjectItem();
    void ClearProjectAssetClipboard();
    void CopySelectedProjectItemToClipboard();
    bool PasteClipboardIntoCurrentFolder();
    bool DuplicateProjectItem(const NLS::Editor::Assets::AssetBrowserItem& item);
    bool DeleteProjectItem(const NLS::Editor::Assets::AssetBrowserItem& item);
    bool RenameProjectItem(const NLS::Editor::Assets::AssetBrowserItem& item, const std::string& newName);
    bool ImportExternalFilesIntoCurrentFolder(const std::vector<std::filesystem::path>& sourcePaths);
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
    struct ThumbnailTextureHandle;
    ThumbnailTextureHandle ResolveCachedThumbnailTextureHandle(
        const std::filesystem::path& imagePath,
        bool queueIfMissing = true);
    void* ResolveAssetBrowserTextureHandle(
        NLS::Render::Resources::Texture2D* texture,
        const std::string& debugName);
    bool LoadCachedThumbnailTexture(const std::string& normalizedPath);
    bool LoadDecodedCachedThumbnailTexture(ThumbnailTextureDecodeResult result);
    void MarkCachedThumbnailTextureUploadRetryableFailure(const std::string& normalizedPath);
    void ApplyThumbnailServiceResult(
        const NLS::Editor::Assets::AssetThumbnailServiceResult& generated);
	void PumpThumbnailGeneration(
		bool allowGpuPreviewStart,
		bool allowHeavyGpuPreview,
		bool allowPreviewRenderWarmup);
	bool EnsureThumbnailPreviewRenderer();
	bool IsEditorWindowClosing() const;
	bool IsEditorSceneReadbackValidationActive() const;
	bool IsStandardPbrShaderPassPrewarmPending() const;
    static ThumbnailTextureDecodeResult DecodeCachedThumbnailTexture(std::string normalizedPath);
    void QueueCachedThumbnailTextureLoad(const std::filesystem::path& imagePath);
    void PumpQueuedCachedThumbnailTextureLoads(size_t maxDecodeStartsPerFrame);
    void StartQueuedCachedThumbnailTextureDecodes(size_t maxDecodeStartsPerFrame);
    void ConsumeCompletedCachedThumbnailTextureDecodes();
    void PumpCurrentFolderItemsRefresh();
    void PumpProjectFolderTreeRefresh();
    void RefreshProjectAssetSubAssetSnapshotCache();
    void StartCurrentFolderItemsRefresh(
        const std::filesystem::path& projectRoot,
        std::string selectedFolder,
        NLS::Editor::Assets::AssetBrowserBuildOptions buildOptions);
    void StartNextCurrentFolderItemsRefresh();
    void StartProjectFolderTreeRefresh(
        const std::filesystem::path& projectRoot,
        NLS::Editor::Assets::AssetBrowserFolderTreeBuildOptions treeOptions);
    void DiscardProjectFolderTreeRefresh();
    void PumpRetiredProjectAssetDatabaseRefreshes();
    static std::shared_ptr<AssetDatabaseRetirementState> SharedProjectAssetDatabaseRetirementState();
    void AbandonProjectAssetDatabaseRefreshFuture(std::future<AssetDatabaseRefreshResult>& future);
    void RetireProjectAssetDatabaseResult(AssetDatabaseRefreshResult result);
    void ScheduleProjectAssetDatabaseRetirementWorker();
    void RetireCurrentProjectAssetDatabase();
    void DiscardProjectAssetDatabaseRefresh();
    void PumpObjectReferencePickerEntriesRefresh();
    void RequestObjectReferencePickerEntriesRefresh();
    void StartNextObjectReferencePickerEntriesRefresh();
    void InvalidateObjectReferencePickerEntriesRefresh();
    void ReleaseAssetBrowserTextureHandleCache(bool force);
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
    std::optional<NLS::Editor::Assets::AssetBrowserActionIdentity> m_selectedProjectItem;
    std::string m_projectSearchQuery;
    NLS::Editor::Assets::AssetBrowserItemType m_projectTypeFilter = NLS::Editor::Assets::AssetBrowserItemType::All;
    float m_thumbnailSize = 96.0f;
    NLS::Editor::Assets::AssetBrowserFolderNode m_projectFolderTree;
    std::vector<NLS::Editor::Assets::AssetBrowserItem> m_unfilteredCurrentFolderItems;
    std::vector<NLS::Editor::Assets::AssetBrowserItem> m_currentFolderItems;
    std::shared_ptr<const NLS::Editor::Assets::EditorAssetSnapshotIndex> m_projectAssetSubAssetSnapshotIndex;
    std::vector<NLS::Editor::Assets::AssetBrowserDisplayItem> m_projectDisplayItems;
    std::vector<NLS::Editor::Assets::AssetBrowserBreadcrumbSegment> m_currentBreadcrumb;
    NLS::Editor::Assets::AssetThumbnailService m_thumbnailService;
    std::shared_ptr<NLS::Editor::Assets::EditorThumbnailPreviewRenderer> m_thumbnailPreviewRenderer;
    double m_nextGpuThumbnailGenerationTime = 0.0;
    double m_heavyGpuThumbnailGenerationDeferredUntil = 0.0;
    double m_nextHeavyGpuThumbnailGenerationTime = 0.0;
    double m_assetBrowserInteractiveUntil = 0.0;
    std::vector<NLS::Editor::Assets::AssetBrowserItem> m_visibleThumbnailItems;
    bool m_visibleThumbnailItemsKnown = false;
    bool m_visiblePrefabHotCachePreloadPending = false;
    std::string m_visibleThumbnailScopeKey;
    uint64_t m_visibleThumbnailFingerprint = 0u;
    size_t m_visibleThumbnailCount = 0u;
    uint32_t m_visibleThumbnailRequestSize = 0u;
    std::vector<NLS::Editor::Assets::AssetBrowserItem> m_pendingThumbnailScopeItems;
    size_t m_pendingThumbnailScopeOffset = 0u;
    NLS::Editor::Assets::AssetThumbnailRequestBuildContext m_pendingThumbnailRequestContext;
    bool m_thumbnailScopeBuildInProgress = false;
    bool m_projectDisplayItemsDirty = true;
    std::unordered_map<std::string, NLS::Editor::Assets::AssetThumbnailServiceResult> m_thumbnailResultsByItemKey;
    std::unordered_map<std::string, std::vector<std::string>> m_thumbnailItemKeyByCacheKey;
    struct ThumbnailTextureCacheEntry
    {
        NLS::Render::Resources::Texture2D* texture = nullptr;
        std::shared_ptr<NLS::Render::RHI::RHITextureView> textureView;
        void* textureId = nullptr;
        uint32_t width = 0u;
        uint32_t height = 0u;
        uint64_t lastUsedFrame = 0u;
    };
    struct ThumbnailTextureHandle
    {
        void* textureHandle = nullptr;
        uint32_t width = 0u;
        uint32_t height = 0u;
    };
    struct ThumbnailTextureDecodeResult
    {
        std::string normalizedPath;
        std::vector<uint8_t> rgbaPixels;
        uint32_t width = 0u;
        uint32_t height = 0u;
    };
    struct PendingThumbnailTextureUpload
    {
        uint64_t requestId = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
    };
    struct AssetBrowserTextureHandleCacheEntry
    {
        std::shared_ptr<NLS::Render::RHI::RHITextureView> textureView;
        void* textureId = nullptr;
    };
    struct InFlightThumbnailTextureDecode
    {
        std::string normalizedPath;
        std::future<ThumbnailTextureDecodeResult> future;
    };
    struct ObjectReferencePickerRefreshKey
    {
        const NLS::Editor::Assets::EditorAssetSnapshotIndex* snapshotIndexIdentity = nullptr;
        const void* lifetimeIdentity = nullptr;

        bool operator==(const ObjectReferencePickerRefreshKey&) const = default;
    };
    struct ObjectReferencePickerRefreshRequest
    {
        ObjectReferencePickerRefreshKey key;
        std::shared_ptr<const NLS::Editor::Assets::EditorAssetSnapshotIndex> snapshotIndex;
    };
    struct ObjectReferencePickerRefresh
    {
        ObjectReferencePickerRefreshKey key;
        struct Result
        {
            std::vector<NLS::Editor::Assets::ObjectReferencePickerEntry> entries;
            std::string diagnostic;
        };
        std::future<Result> future;
    };

    struct AssetDatabaseRefreshResult
    {
        std::unique_ptr<NLS::Editor::Assets::AssetDatabaseFacade> database;
        std::shared_ptr<const NLS::Editor::Assets::AssetDatabaseFacade> snapshot;
        std::string diagnostic;
    };
    struct AssetDatabaseRetirementState
    {
        std::mutex mutex;
        std::vector<AssetDatabaseRefreshResult> pending;
        std::vector<std::future<AssetDatabaseRefreshResult>> pendingFutures;
        bool workerRunning = false;
    };
    struct AssetDatabaseRefresh
    {
        std::filesystem::path root;
        std::future<AssetDatabaseRefreshResult> future;
    };
    struct CurrentFolderItemsRefreshKey
    {
        std::filesystem::path projectRoot;
        std::string selectedFolder;
        const NLS::Editor::Assets::AssetDatabaseFacade* databaseSnapshotIdentity = nullptr;
        const NLS::Editor::Assets::EditorAssetSnapshotIndex* snapshotIndexIdentity = nullptr;
        std::shared_ptr<const uint8_t> sourceStateIdentity;
        NLS::Editor::Assets::AssetBrowserBuildOptions buildOptions;

        bool operator==(const CurrentFolderItemsRefreshKey&) const = default;
    };
    struct CurrentFolderItemsRefreshRequest
    {
        CurrentFolderItemsRefreshKey key;
        std::shared_ptr<const NLS::Editor::Assets::AssetDatabaseFacade> databaseSnapshot;
        std::shared_ptr<const NLS::Editor::Assets::EditorAssetSnapshotIndex> snapshotIndex;
    };
    struct CurrentFolderItemsRefresh
    {
        struct Result
        {
            NLS::Editor::Assets::AssetBrowserPresentationBundle bundle;
            std::string diagnostic;
        };
        CurrentFolderItemsRefreshKey key;
        std::future<Result> future;
    };
    struct ProjectFolderTreeRefresh
    {
        uint64_t generation = 0u;
        std::filesystem::path projectRoot;
        std::string selectedFolder;
        std::future<NLS::Editor::Assets::AssetBrowserFolderNode> future;
    };
    struct WatcherStartupResult
    {
        Core::AssetFileWatcher engineAssetsWatcher;
        Core::AssetFileWatcher projectAssetsWatcher;
        NLS::Core::Assets::AssetDiagnostics diagnostics;
    };
    struct HoveredPrefabHotCachePreloadIdentity
    {
        std::string dragResourcePath;
        NLS::Core::Assets::AssetId assetId;
        std::string subAssetKey;
        NLS::Editor::Assets::AssetBrowserItemKind kind = NLS::Editor::Assets::AssetBrowserItemKind::Folder;
        NLS::Editor::Assets::AssetBrowserItemType type = NLS::Editor::Assets::AssetBrowserItemType::Other;
        NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;

        bool Matches(const NLS::Editor::Assets::AssetBrowserItem& item) const
        {
            return item.dragResourcePath == dragResourcePath &&
                item.assetId == assetId &&
                item.subAssetKey == subAssetKey &&
                item.kind == kind &&
                item.type == type &&
                item.artifactType == artifactType;
        }

        void Store(const NLS::Editor::Assets::AssetBrowserItem& item)
        {
            dragResourcePath = item.dragResourcePath;
            assetId = item.assetId;
            subAssetKey = item.subAssetKey;
            kind = item.kind;
            type = item.type;
            artifactType = item.artifactType;
        }
    };
    std::unordered_map<std::string, ThumbnailTextureCacheEntry> m_thumbnailTexturesByPath;
    std::unordered_map<std::string, PendingThumbnailTextureUpload> m_pendingThumbnailTextureUploadsByPath;
    std::string m_nextPendingThumbnailTextureUploadPollPath;
    std::unordered_map<std::string, uint64_t> m_thumbnailTextureRetryAfterFrameByPath;
    std::vector<std::string> m_thumbnailTextureLru;
    std::unordered_set<std::string> m_thumbnailTexturesUsedThisFrame;
    std::unordered_set<std::string> m_thumbnailTexturesPendingRelease;
    std::vector<std::string> m_thumbnailTextureLoadQueue;
    std::unordered_set<std::string> m_thumbnailTexturesQueuedForLoad;
    std::unordered_set<std::string> m_thumbnailTexturesFailedToLoad;
    std::vector<InFlightThumbnailTextureDecode> m_thumbnailTextureDecodes;
    std::unordered_set<std::string> m_thumbnailTexturesDecoding;
    std::unordered_map<
        NLS::Render::Resources::Texture2D*,
        AssetBrowserTextureHandleCacheEntry> m_assetBrowserTextureHandleCache;
    std::future<size_t> m_standardPbrShaderPassPrewarm;
    bool m_standardPbrShaderPassPrewarmQueued = false;
    bool m_standardPbrShaderPassPrewarmCompleted = false;
    bool m_thumbnailPreviewRenderWarmupCompleted = false;
    double m_lightGpuThumbnailGenerationDeferredUntil = 0.0;
    uint64_t m_thumbnailTextureFrameSerial = 0u;
    std::optional<ObjectReferencePickerRefresh> m_objectReferencePickerRefresh;
    std::optional<ObjectReferencePickerRefreshRequest> m_pendingObjectReferencePickerRefresh;
    NLS::Editor::Assets::AssetBrowserLatestRequestCoordinator<ObjectReferencePickerRefreshKey>
        m_objectReferencePickerRefreshCoordinator;
    std::shared_ptr<const uint8_t> m_objectReferencePickerLifetimeIdentity = std::make_shared<const uint8_t>(0u);
    NLS::Editor::Assets::AssetBrowserAsyncRefreshState m_objectReferencePickerRefreshState;
    bool m_objectReferencePickerRefreshRequested = false;
    uint32_t m_lastThumbnailRequestSize = 0u;
    std::string m_lastThumbnailGenerationScopeKey;
    bool m_lastThumbnailGenerationScopeInteractive = false;
    HoveredPrefabHotCachePreloadIdentity m_lastHoveredPrefabHotCachePreloadIdentity;
    double m_lastHoveredPrefabHotCachePreloadTime = 0.0;
    bool m_thumbnailGenerationScopeDirty = true;
    std::unique_ptr<NLS::Editor::Assets::AssetDatabaseFacade> m_projectAssetDatabase;
    std::shared_ptr<const NLS::Editor::Assets::AssetDatabaseFacade> m_projectAssetDatabaseSnapshot;
    std::filesystem::path m_projectAssetDatabaseRoot;
    bool m_projectAssetDatabaseReady = false;
    std::optional<AssetDatabaseRefresh> m_projectAssetDatabaseRefresh;
    std::vector<AssetDatabaseRefresh> m_retiredProjectAssetDatabaseRefreshes;
    NLS::Editor::Assets::AssetBrowserAsyncRefreshState m_projectAssetDatabaseRefreshState;
    std::shared_ptr<AssetDatabaseRetirementState> m_projectAssetDatabaseRetirementState =
        SharedProjectAssetDatabaseRetirementState();
    bool m_projectAssetDatabaseRefreshQueuedAfterInFlight = false;
    std::optional<CurrentFolderItemsRefresh> m_currentFolderItemsRefresh;
    std::optional<CurrentFolderItemsRefreshRequest> m_pendingCurrentFolderItemsRefresh;
    NLS::Editor::Assets::AssetBrowserLatestRequestCoordinator<CurrentFolderItemsRefreshKey>
        m_currentFolderItemsRefreshCoordinator;
    NLS::Editor::Assets::AssetBrowserAsyncRefreshState m_currentFolderItemsRefreshState;
    std::optional<ProjectFolderTreeRefresh> m_projectFolderTreeRefresh;
    std::vector<ProjectFolderTreeRefresh> m_retiredProjectFolderTreeRefreshes;
    ProjectBrowserTextDialogState m_projectBrowserTextDialog;
    std::pair<std::string, UI::Widgets::Group*> m_projectGridDragPairPayload;
    std::vector<std::filesystem::path> m_projectAssetClipboardPaths;
    bool m_projectAssetClipboardCut = false;
    ProjectBrowserInlineRenameState m_projectBrowserInlineRename;
    bool m_projectDeleteShortcutAwaitingRelease = false;
    bool m_projectDeleteActionAwaitingRelease = false;
    double m_projectDeleteActionSuppressedUntil = 0.0;
    NLS::Editor::Assets::AssetBrowserExternalDroppedFileQueue m_pendingExternalDroppedFiles;
    uint64_t m_windowDroppedFilesListener = 0u;
    std::unordered_map<UI::Widgets::TreeNode*, std::string> m_pathUpdate;
    std::unordered_set<std::string> m_expandedFolders;
    std::unordered_set<std::string> m_expandedProjectFolders;
    std::unordered_set<std::string> m_expandedProjectAssetItems;
    Core::AssetFileWatcher m_engineAssetsWatcher;
    Core::AssetFileWatcher m_projectAssetsWatcher;
    std::future<WatcherStartupResult> m_watcherStartup;
    bool m_watchersStartupQueued = false;
    bool m_watchersReadyRefreshQueued = false;
    bool m_refreshRequested = false;
    double m_refreshRequestedAfter = 0.0;
    bool m_projectFolderTreeRefreshRequested = false;
    bool m_startupWatcherPreimportGateOpen = true;
    bool m_projectAssetPreimportRunning = false;
    uint64_t m_projectAssetPresentationGeneration = 0u;
    uint64_t m_projectFolderTreeRefreshGeneration = 0u;
    std::optional<NLS::Editor::Assets::AssetPreimportRequest> m_pendingProjectAssetPreimportRequest;
};
} // namespace NLS::Editor::Panels
