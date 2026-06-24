#pragma once

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetDiagnostics.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Utils/PathParser.h"

#include <array>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <vector>

namespace NLS::Editor::Assets
{
enum class AssetBrowserItemKind
{
    Folder,
    SourceAsset,
    GeneratedSubAsset
};

enum class AssetBrowserItemType
{
    All,
    Folder,
    Model,
    Prefab,
    Mesh,
    Material,
    Texture,
    Shader,
    Scene,
    Script,
    Other,
    Count
};

inline constexpr size_t kAssetBrowserItemTypeCount = static_cast<size_t>(AssetBrowserItemType::Count);

struct AssetBrowserFolderNode
{
    std::string displayName;
    std::string projectRelativePath;
    std::filesystem::path absolutePath;
    std::vector<AssetBrowserFolderNode> children;
    bool hasChildren = false;
    bool childrenEnumerated = false;
};

struct AssetBrowserItem
{
    std::string displayName;
    std::string projectRelativePath;
    std::string sourceAssetPath;
    std::filesystem::path absolutePath;
    std::string artifactPath;
    AssetBrowserItemKind kind = AssetBrowserItemKind::SourceAsset;
    AssetBrowserItemType type = AssetBrowserItemType::Other;
    NLS::Core::Assets::AssetId assetId;
    std::string subAssetKey;
    std::string dragResourcePath;
    std::string selectionResourcePath;
    NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    bool generatedReadOnly = false;
    bool previewableInAssetView = false;
    bool hasGeneratedSubAssets = false;
};

struct AssetBrowserBreadcrumbSegment
{
    std::string displayName;
    std::string projectRelativePath;
};

struct AssetBrowserFolderSelection
{
    std::string projectRelativePath = "Assets";
    std::filesystem::path absolutePath;
    bool exists = false;
};

struct AssetBrowserBuildOptions
{
    bool includeGeneratedSubAssets = false;
    bool verifyGeneratedSubAssetManifests = true;
    bool loadSourceAssetMetadataWithoutDatabase = true;
    std::unordered_set<std::string> expandedSourceAssets;
    std::unordered_map<std::string, size_t> generatedSubAssetCountHints;
    std::string searchQuery;
    AssetBrowserItemType typeFilter = AssetBrowserItemType::All;
};

struct AssetBrowserFolderTreeBuildOptions
{
    std::unordered_set<std::string> expandedFolders;
    std::string selectedFolder = "Assets";
};

enum class AssetBrowserRefreshReason
{
    InitialBuild,
    AssetDatabaseMutation,
    AssetDatabaseReady,
    FolderSelection,
    FilterChange,
    Count
};

inline constexpr size_t kAssetBrowserRefreshReasonCount = static_cast<size_t>(AssetBrowserRefreshReason::Count);

struct AssetBrowserRefreshPlan
{
    bool refreshAssetDatabase = false;
    bool rebuildFolderTree = false;
    bool rebuildCurrentFolderItems = true;
};

struct AssetDatabaseRefreshSchedulingDecision
{
    bool startRefresh = false;
    bool queueRefreshAfterInFlight = false;
};

using AssetBrowserExternalDroppedFileBatch = std::vector<std::string>;
using AssetBrowserExternalDroppedFileQueue = std::deque<AssetBrowserExternalDroppedFileBatch>;

enum class AssetDatabaseRefreshDiscardAction
{
    Drop,
    Retire
};

AssetBrowserRefreshPlan BuildAssetBrowserRefreshPlan(AssetBrowserRefreshReason reason);

void EnqueueAssetBrowserExternalDroppedFiles(
    AssetBrowserExternalDroppedFileQueue& queue,
    AssetBrowserExternalDroppedFileBatch paths);

std::optional<AssetBrowserExternalDroppedFileBatch> ConsumeAssetBrowserExternalDroppedFiles(
    AssetBrowserExternalDroppedFileQueue& queue,
    bool currentFolderHovered);

AssetDatabaseRefreshSchedulingDecision PlanAssetDatabaseRefreshScheduling(
    bool projectRootEmpty,
    bool refreshRequested,
    bool databaseReady,
    bool refreshInFlightForSameRoot);

AssetDatabaseRefreshDiscardAction PlanAssetDatabaseRefreshDiscardAction(
    bool futureValid,
    bool futureReady);

std::string NormalizeAssetBrowserProjectRelativePath(std::string path);

bool ShouldStopDrawingAssetBrowserFolderNodeAfterSelection(
    const std::string& selectedFolder,
    const std::string& clickedFolder);

bool ShouldStopDrawingAssetBrowserGridAfterOpeningItem(
    const std::string& selectedFolder,
    const AssetBrowserItem& openedItem);

struct AssetBrowserWorkflowCapabilities
{
    bool canShowInExplorer = false;
    bool canImportHere = false;
    bool canCreateChildren = false;
    bool canRename = false;
    bool canDelete = false;
    bool canDuplicate = false;
    bool canOpenExternal = false;
    bool canReimport = false;
    bool canReload = false;
    bool canCompile = false;
    bool canPreview = false;
    bool canEdit = false;
    bool canOpenProperties = false;
    bool canAcceptAssetDrops = false;
    bool canAcceptHierarchyDrops = false;
};

struct AssetBrowserItemTypeColor
{
    uint8_t red = 135u;
    uint8_t green = 142u;
    uint8_t blue = 150u;
    uint8_t alpha = 255u;
};

enum class AssetBrowserContentViewMode
{
    Grid,
    List
};

struct AssetBrowserDisplayItem
{
    AssetBrowserItem item;
    size_t childCount = 0u;
    bool subAsset = false;
    bool expanded = false;
    bool loadingPlaceholder = false;
};

struct AssetBrowserPoint
{
    float x = 0.0f;
    float y = 0.0f;
};

struct AssetBrowserRect
{
    AssetBrowserPoint min;
    AssetBrowserPoint max;
};

struct AssetBrowserDisplayItemRange
{
    size_t begin = 0u;
    size_t count = 0u;
};

AssetBrowserWorkflowCapabilities BuildAssetBrowserWorkflowCapabilities(
    const AssetBrowserItem& item);

AssetBrowserItemType AssetBrowserItemTypeFromPathParserFileType(
    NLS::Utils::PathParser::EFileType fileType);

bool AssetBrowserSourceAssetCanHaveGeneratedSubAssets(const std::string& sourceAssetPath);

const char* AssetBrowserItemTypeDisplayLabel(AssetBrowserItemType type);

AssetBrowserItemTypeColor AssetBrowserItemTypeDisplayColor(AssetBrowserItemType type);

const char* AssetBrowserFallbackIconId(AssetBrowserItemType type);

std::string_view ResolveAssetBrowserDisplayFallbackIconId(
    AssetBrowserItemType type,
    std::string_view thumbnailFallbackIcon);

AssetBrowserContentViewMode ResolveAssetBrowserContentViewMode(float thumbnailSize);

AssetBrowserRect ComputeAssetBrowserThumbnailRect(
    AssetBrowserRect bounds,
    uint32_t imageWidth,
    uint32_t imageHeight);

bool ShouldDrawAssetBrowserThumbnailLetterboxBackground(AssetBrowserItemType type);

std::vector<AssetBrowserDisplayItem> BuildAssetBrowserDisplayItems(
    const std::vector<AssetBrowserItem>& items,
    const std::unordered_set<std::string>& expandedSourceAssets,
    const std::unordered_map<std::string, size_t>& generatedSubAssetCountHints = {});

std::vector<AssetBrowserDisplayItem> BuildProgressiveAssetBrowserDisplayItems(
    const std::vector<AssetBrowserDisplayItem>& displayItems,
    const std::unordered_map<std::string, size_t>& generatedSubAssetRevealCounts,
    size_t maxGeneratedSubAssetPlaceholdersPerSource);

std::optional<AssetBrowserDisplayItemRange> ResolveAssetBrowserExpandedSubAssetRange(
    const std::vector<AssetBrowserDisplayItem>& displayItems,
    size_t sourceDisplayIndex);

const std::array<AssetBrowserItemType, kAssetBrowserItemTypeCount>& AssetBrowserItemTypeFilterOptions();

std::string BuildAssetBrowserThumbnailGenerationScopeKey(
    const std::string& selectedFolder,
    uint32_t requestedSize,
    const std::vector<AssetBrowserItem>& visibleItems);

std::string BuildAssetBrowserThumbnailItemKey(
    const AssetBrowserItem& item,
    uint32_t requestedSize);

template<typename ItemKeyMap>
void RegisterAssetBrowserThumbnailCacheKeyBinding(
    ItemKeyMap& itemKeysByCacheKey,
    const std::string& cacheKey,
    const std::string& itemKey)
{
    auto& itemKeys = itemKeysByCacheKey[cacheKey];
    if (std::find(itemKeys.begin(), itemKeys.end(), itemKey) == itemKeys.end())
        itemKeys.push_back(itemKey);
}

template<typename ItemKeyMap, typename ResultMap, typename Result>
void ApplyAssetBrowserThumbnailCacheKeyResult(
    const ItemKeyMap& itemKeysByCacheKey,
    ResultMap& resultsByItemKey,
    const std::string& cacheKey,
    const Result& result)
{
    const auto foundItemKeys = itemKeysByCacheKey.find(cacheKey);
    if (foundItemKeys == itemKeysByCacheKey.end())
        return;

    for (const auto& itemKey : foundItemKeys->second)
        resultsByItemKey[itemKey] = result;
}

struct AssetBrowserThumbnailGenerationScopeDecision
{
    bool canSkip = false;
    bool scopeChanged = false;
    bool requerySameScope = false;
};

struct AssetBrowserHeavyGpuThumbnailPumpInput
{
    bool allowHeavyGpuPreview = true;
    bool interactive = false;
    bool hasQueuedWork = false;
    bool hasInFlightWork = false;
    bool hasPreviewRenderer = false;
    double nowSeconds = 0.0;
    double deferredUntilSeconds = 0.0;
    double nextAllowedSeconds = 0.0;
};

struct AssetBrowserHeavyGpuThumbnailPumpDecision
{
    bool shouldPump = false;
};

struct AssetBrowserLightGpuThumbnailPumpInput
{
    bool allowGpuPreviewStart = true;
    bool interactive = false;
    bool hasQueuedWork = false;
    bool hasInFlightWork = false;
    bool hasPreviewRenderer = false;
    double nowSeconds = 0.0;
    double nextAllowedSeconds = 0.0;
};

struct AssetBrowserLightGpuThumbnailPumpDecision
{
    bool shouldPump = false;
};

struct AssetBrowserThumbnailPumpInput
{
    bool interactive = false;
    bool hasQueuedWork = false;
    bool hasInFlightWork = false;
    size_t interactiveStartsThisFrame = 0u;
    size_t maxInteractiveStartsPerFrame = 1u;
};

struct AssetBrowserThumbnailPumpDecision
{
    bool shouldStartBackgroundWork = false;
};

struct AssetBrowserCachedThumbnailTexturePumpInput
{
    bool interactive = false;
    size_t queuedTextureLoads = 0u;
    size_t inFlightDecodes = 0u;
    size_t interactiveStartsThisFrame = 0u;
    size_t maxInteractiveStartsPerFrame = 1u;
};

struct AssetBrowserCachedThumbnailTexturePumpDecision
{
    bool shouldPump = false;
};

AssetBrowserThumbnailGenerationScopeDecision EvaluateAssetBrowserThumbnailGenerationScope(
    const std::string& previousScopeKey,
    uint32_t previousRequestedSize,
    bool scopeDirty,
    const std::string& nextScopeKey,
    uint32_t nextRequestedSize);

AssetBrowserHeavyGpuThumbnailPumpDecision PlanAssetBrowserHeavyGpuThumbnailPump(
    const AssetBrowserHeavyGpuThumbnailPumpInput& input);

AssetBrowserLightGpuThumbnailPumpDecision PlanAssetBrowserLightGpuThumbnailPump(
    const AssetBrowserLightGpuThumbnailPumpInput& input);

AssetBrowserThumbnailPumpDecision PlanAssetBrowserThumbnailPump(
    const AssetBrowserThumbnailPumpInput& input);

AssetBrowserCachedThumbnailTexturePumpDecision PlanAssetBrowserCachedThumbnailTexturePump(
    const AssetBrowserCachedThumbnailTexturePumpInput& input);

AssetBrowserFolderNode BuildProjectAssetFolderTree(const std::filesystem::path& projectRootOrAssetsRoot);

AssetBrowserFolderNode BuildProjectAssetFolderTree(
    const std::filesystem::path& projectRootOrAssetsRoot,
    const AssetBrowserFolderTreeBuildOptions& options);

std::vector<AssetBrowserItem> BuildCurrentFolderAssetItems(
    const std::filesystem::path& projectRootOrAssetsRoot,
    const std::string& selectedFolder,
    const AssetDatabaseFacade* database = nullptr);

std::vector<AssetBrowserItem> BuildCurrentFolderAssetItems(
    const std::filesystem::path& projectRootOrAssetsRoot,
    const std::string& selectedFolder,
    const AssetDatabaseFacade* database,
    AssetBrowserBuildOptions options);

std::vector<AssetBrowserItem> FilterAssetBrowserItems(
    const std::vector<AssetBrowserItem>& items,
    const AssetBrowserBuildOptions& options);

std::vector<std::string> SelectAssetBrowserThumbnailTextureEvictionCandidates(
    const std::vector<std::string>& lruKeys,
    const std::unordered_set<std::string>& usedThisFrame,
    size_t maxResidentTextures);

std::vector<std::string> SelectAssetBrowserThumbnailTextureLoadCandidates(
    const std::vector<std::string>& queuedKeys,
    const std::unordered_set<std::string>& residentKeys,
    size_t maxLoads);

std::vector<std::string> SelectAssetBrowserThumbnailTextureDecodeCandidates(
    const std::vector<std::string>& queuedKeys,
    const std::unordered_set<std::string>& residentKeys,
    const std::unordered_set<std::string>& decodingKeys,
    size_t maxDecodes);

size_t AssetBrowserThumbnailTextureDecodeStartBudget(
    size_t inFlightDecodes,
    size_t maxInFlightDecodes);

bool IsAssetBrowserCachedThumbnailTextureSizeAllowed(
    uint32_t width,
    uint32_t height,
    uint32_t maxDimension = 512u);

struct AssetBrowserThumbnailTextureFrameReleasePlan
{
    std::unordered_set<std::string> usedThisFrame;
    std::unordered_set<std::string> pendingRelease;
    std::vector<std::string> releaseNow;
};

AssetBrowserThumbnailTextureFrameReleasePlan BeginAssetBrowserThumbnailTextureFrame(
    std::unordered_set<std::string> usedThisFrame,
    std::unordered_set<std::string> pendingRelease);

AssetBrowserThumbnailTextureFrameReleasePlan PlanAssetBrowserThumbnailTextureFullClear(
    const std::vector<std::string>& residentKeys,
    const std::unordered_set<std::string>& usedThisFrame,
    const std::unordered_set<std::string>& pendingRelease);

std::vector<AssetBrowserItem> SelectAssetBrowserThumbnailGenerationItems(
    const std::vector<AssetBrowserItem>& currentFolderItems,
    const std::vector<AssetBrowserItem>& visibleItems,
    bool visibleItemsKnown);

std::vector<AssetBrowserBreadcrumbSegment> BuildAssetBrowserBreadcrumb(const std::string& selectedFolder);

AssetBrowserFolderSelection ResolveAssetBrowserFolderSelection(
    const std::filesystem::path& projectRootOrAssetsRoot,
    const std::string& requestedFolder);

bool CanMovePhysicalProjectAssetFileIntoFolder(
    const std::filesystem::path& assetsRoot,
    const std::filesystem::path& sourceFile,
    const std::filesystem::path& targetFolder);

bool CanMovePhysicalProjectAssetFolderIntoFolder(
    const std::filesystem::path& assetsRoot,
    const std::filesystem::path& sourceFolder,
    const std::filesystem::path& targetFolder);

bool CanMoveProjectBrowserResourcePathIntoFolder(
    const std::filesystem::path& assetsRoot,
    const std::string& sourceResourcePath,
    const std::filesystem::path& targetFolder,
    bool sourceIsFolder);

std::optional<EditorAssetDragPayload> MakeAssetBrowserItemDragPayload(
    const AssetBrowserItem& item,
    const AssetDatabaseFacade* database);

struct AssetBrowserSubAssetEntry
{
    std::string displayName;
    std::string sourceAssetPath;
    std::string subAssetKey;
    std::string dragResourcePath;
    NLS::Core::Assets::AssetId assetId;
    NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    bool generatedReadOnly = false;
};

std::vector<AssetBrowserSubAssetEntry> BuildAssetBrowserSubAssetEntries(
    const AssetDatabaseFacade& database,
    const std::string& sourceAssetPath);

struct ObjectReferencePickerEntry
{
    std::string displayName;
    EditorAssetDragPayload payload {};
};

using ObjectReferencePickerEntriesProvider = std::function<std::vector<ObjectReferencePickerEntry>()>;

std::vector<ObjectReferencePickerEntry> BuildObjectReferencePickerEntriesFromSnapshots(
    const std::vector<ObjectReferencePickerAssetSnapshot>& snapshots);

std::vector<ObjectReferencePickerEntry> BuildObjectReferencePickerEntries(
    const AssetDatabaseFacade& database);

void SetObjectReferencePickerAssetRoots(std::vector<EditorAssetRoot> roots);
std::vector<EditorAssetRoot> GetObjectReferencePickerAssetRoots();
void SetObjectReferencePickerEntries(std::vector<ObjectReferencePickerEntry> entries);
void SetObjectReferencePickerEntriesProvider(ObjectReferencePickerEntriesProvider provider);
void MarkObjectReferencePickerEntriesDirty();
bool ShouldDeferObjectReferencePickerEntriesRefresh();
bool RefreshObjectReferencePickerEntries();
std::vector<ObjectReferencePickerEntry> GetObjectReferencePickerEntries();

struct AssetWatcherStartupReport
{
    bool succeeded = true;
    NLS::Core::Assets::AssetDiagnostics diagnostics;
};

AssetWatcherStartupReport BuildAssetWatcherStartupReport(
    const std::filesystem::path& engineAssetsPath,
    bool engineWatcherStarted,
    const std::filesystem::path& projectAssetsPath,
    bool projectWatcherStarted);
}
