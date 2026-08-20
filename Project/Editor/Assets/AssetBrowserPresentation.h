#pragma once

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetDiagnostics.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/AssetThumbnailCache.h"
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
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <utility>
#include <vector>

namespace NLS::Editor::Assets
{
struct AssetThumbnailServiceResult;

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

    bool operator==(const AssetBrowserItem&) const = default;
};

struct AssetBrowserActionIdentity
{
    AssetBrowserItemKind kind = AssetBrowserItemKind::Folder;
    std::string canonicalSourcePath;
    NLS::Core::Assets::AssetId assetId;
    std::string subAssetKey;

    bool operator==(const AssetBrowserActionIdentity&) const = default;
};

std::optional<AssetBrowserActionIdentity> BuildAssetBrowserActionIdentity(
    const AssetBrowserItem& item);

bool AssetBrowserItemMatchesActionIdentity(
    const AssetBrowserItem& item,
    const AssetBrowserActionIdentity& identity);

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
    std::string searchQuery;
    AssetBrowserItemType typeFilter = AssetBrowserItemType::All;

    bool operator==(const AssetBrowserBuildOptions&) const = default;
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
    bool clearCurrentFolderItemsBeforeAsyncRefresh = false;
};

struct AssetDatabaseRefreshSchedulingDecision
{
    bool startRefresh = false;
    bool queueRefreshAfterInFlight = false;
};

enum class AssetBrowserAsyncRefreshStatus
{
    Idle,
    Loading,
    Success,
    Failure,
    Closed
};

struct AssetBrowserAsyncRefreshState
{
    AssetBrowserAsyncRefreshStatus status = AssetBrowserAsyncRefreshStatus::Idle;
    std::string diagnostic;
};

void ResetAssetBrowserAsyncRefresh(AssetBrowserAsyncRefreshState& state);
void BeginAssetBrowserAsyncRefresh(AssetBrowserAsyncRefreshState& state);
void CompleteAssetBrowserAsyncRefresh(AssetBrowserAsyncRefreshState& state);
void FailAssetBrowserAsyncRefresh(AssetBrowserAsyncRefreshState& state, std::string diagnostic);
void CloseAssetBrowserAsyncRefresh(AssetBrowserAsyncRefreshState& state);

enum class AssetBrowserLatestRequestDisposition
{
    StartNow,
    ActiveUnchanged,
    PendingReplaced
};

template<typename Key>
struct AssetBrowserLatestRequestCoordinator
{
    std::optional<Key> desiredKey;
    std::optional<Key> activeKey;
    std::optional<Key> pendingKey;
};

template<typename Key>
struct AssetBrowserLatestRequestCompletion
{
    bool publish = false;
    std::optional<Key> nextKey;
};

template<typename Key>
AssetBrowserLatestRequestDisposition QueueAssetBrowserLatestRequest(
    AssetBrowserLatestRequestCoordinator<Key>& coordinator,
    Key key)
{
    coordinator.desiredKey = key;
    if (!coordinator.activeKey.has_value())
    {
        coordinator.pendingKey = std::move(key);
        return AssetBrowserLatestRequestDisposition::StartNow;
    }
    if (*coordinator.activeKey == key)
    {
        coordinator.pendingKey.reset();
        return AssetBrowserLatestRequestDisposition::ActiveUnchanged;
    }
    coordinator.pendingKey = std::move(key);
    return AssetBrowserLatestRequestDisposition::PendingReplaced;
}

template<typename Key>
std::optional<Key> ActivateAssetBrowserLatestRequest(
    AssetBrowserLatestRequestCoordinator<Key>& coordinator)
{
    if (coordinator.activeKey.has_value() || !coordinator.pendingKey.has_value())
        return std::nullopt;
    coordinator.activeKey = std::move(coordinator.pendingKey);
    coordinator.pendingKey.reset();
    return coordinator.activeKey;
}

template<typename Key>
AssetBrowserLatestRequestCompletion<Key> CompleteAssetBrowserLatestRequest(
    AssetBrowserLatestRequestCoordinator<Key>& coordinator,
    const Key& completedKey)
{
    AssetBrowserLatestRequestCompletion<Key> completion;
    if (!coordinator.activeKey.has_value() || *coordinator.activeKey != completedKey)
        return completion;

    completion.publish =
        coordinator.desiredKey.has_value() &&
        *coordinator.desiredKey == completedKey;
    coordinator.activeKey.reset();
    if (coordinator.pendingKey.has_value() &&
        coordinator.desiredKey.has_value() &&
        *coordinator.pendingKey == *coordinator.desiredKey)
    {
        completion.nextKey = coordinator.pendingKey;
    }
    return completion;
}

template<typename Key>
void CloseAssetBrowserLatestRequestCoordinator(AssetBrowserLatestRequestCoordinator<Key>& coordinator)
{
    coordinator.desiredKey.reset();
    coordinator.activeKey.reset();
    coordinator.pendingKey.reset();
}

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
    uint64_t groupId = 0u;
    size_t childCount = 0u;
    bool subAsset = false;
    bool expanded = false;
    bool loadingPlaceholder = false;
};

bool ShouldShowAssetBrowserSubAssetDisclosure(const AssetBrowserDisplayItem& displayItem);

std::vector<AssetBrowserDisplayItem> BuildAssetBrowserImmediateDisclosureFeedback(
    const AssetBrowserDisplayItem& sourceDisplayItem,
    bool expanded);

struct AssetBrowserPresentationBundle
{
    std::vector<AssetBrowserItem> rootItems;
    std::vector<AssetBrowserItem> visibleItems;
    std::vector<AssetBrowserDisplayItem> displayItems;
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

struct AssetBrowserGroupSegment
{
    AssetBrowserDisplayItemRange range;
    bool trueSegmentStart = false;
    bool trueSegmentEnd = false;
};

AssetBrowserWorkflowCapabilities BuildAssetBrowserWorkflowCapabilities(
    const AssetBrowserItem& item);

AssetBrowserItemType AssetBrowserItemTypeFromPathParserFileType(
    NLS::Utils::PathParser::EFileType fileType);

bool AssetBrowserSourceAssetCanHaveGeneratedSubAssets(const std::string& sourceAssetPath);

std::optional<AssetBrowserItemType> AssetBrowserGeneratedArtifactItemType(
    NLS::Core::Assets::ArtifactType artifactType);

const char* AssetBrowserItemTypeDisplayLabel(AssetBrowserItemType type);

AssetBrowserItemTypeColor AssetBrowserItemTypeDisplayColor(AssetBrowserItemType type);

const char* AssetBrowserFallbackIconId(AssetBrowserItemType type);

const char* AssetBrowserScriptIconId(std::string_view sourceAssetPath);

std::string_view ResolveAssetBrowserDisplayFallbackIconId(
    AssetBrowserItemType type,
    std::string_view thumbnailFallbackIcon,
    std::string_view sourceAssetPath = {});

AssetBrowserContentViewMode ResolveAssetBrowserContentViewMode(float thumbnailSize);

AssetBrowserRect ComputeAssetBrowserThumbnailRect(
    AssetBrowserRect bounds,
    uint32_t imageWidth,
    uint32_t imageHeight);

std::vector<AssetBrowserDisplayItem> BuildAssetBrowserDisplayItems(
    const std::vector<AssetBrowserItem>& items,
    const std::unordered_set<std::string>& expandedSourceAssets);

std::vector<AssetBrowserDisplayItem> BuildFilteredAssetBrowserDisplayItems(
    const std::vector<AssetBrowserItem>& items,
    const std::unordered_set<std::string>& expandedSourceAssets,
    const std::string& searchQuery,
    AssetBrowserItemType typeFilter);

AssetBrowserPresentationBundle BuildAssetBrowserPresentationBundle(
    std::vector<AssetBrowserItem> rootItems,
    const EditorAssetSnapshotIndex* snapshotIndex,
    const AssetBrowserBuildOptions& options);

bool ShouldPublishPartialAssetBrowserDisplayRebuild(
    size_t processedSinceLastPublish,
    bool interactive);

std::optional<AssetBrowserDisplayItemRange> ResolveAssetBrowserExpandedSubAssetRange(
    const std::vector<AssetBrowserDisplayItem>& displayItems,
    size_t sourceDisplayIndex);

std::vector<AssetBrowserGroupSegment> ResolveAssetBrowserGridRowGroupSegments(
    const std::vector<AssetBrowserDisplayItem>& displayItems,
    size_t rowBegin,
    size_t columnCount);

std::vector<AssetBrowserGroupSegment> ResolveAssetBrowserVisibleListGroupSegments(
    const std::vector<AssetBrowserDisplayItem>& displayItems,
    size_t visibleBegin,
    size_t visibleEnd);

const std::array<AssetBrowserItemType, kAssetBrowserItemTypeCount>& AssetBrowserItemTypeFilterOptions();

std::string BuildAssetBrowserThumbnailGenerationScopeKey(
    const std::string& selectedFolder,
    uint32_t requestedSize,
    const std::vector<AssetBrowserItem>& visibleItems);

std::string BuildAssetBrowserThumbnailItemKey(
    const AssetBrowserItem& item,
    uint32_t requestedSize);

std::vector<std::filesystem::path> BuildAssetBrowserThumbnailImageCandidates(
    const std::filesystem::path& canonicalImagePath,
    const std::filesystem::path& retainedImagePath,
    const std::filesystem::path& pendingCacheImagePath);

bool AssetBrowserThumbnailItemMatchesResultIdentity(
    const AssetBrowserItem& item,
    const std::string& sourceAssetPath,
    const std::string& subAssetKey,
    AssetThumbnailKind requestKind);

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

template<typename Result, typename = void>
struct AssetBrowserThumbnailResultHasImagePath : std::false_type
{
};

template<typename Result>
struct AssetBrowserThumbnailResultHasImagePath<
    Result,
    std::void_t<decltype(std::declval<const Result&>().imagePath)>> : std::true_type
{
};

template<typename Result, typename = void>
struct AssetBrowserThumbnailResultHasRetainedImage : std::false_type
{
};

template<typename Result>
struct AssetBrowserThumbnailResultHasRetainedImage<
    Result,
    std::void_t<decltype(std::declval<const Result&>().retainedImage)>> : std::true_type
{
};

template<typename Result, typename = void>
struct AssetBrowserThumbnailResultHasCacheEntry : std::false_type
{
};

template<typename Result>
struct AssetBrowserThumbnailResultHasCacheEntry<
    Result,
    std::void_t<decltype(std::declval<const Result&>().cacheEntry)>> : std::true_type
{
};

template<typename Result, typename = void>
struct AssetBrowserThumbnailResultHasGpuTexture : std::false_type
{
};

template<typename Result>
struct AssetBrowserThumbnailResultHasGpuTexture<
    Result,
    std::void_t<decltype(std::declval<const Result&>().gpuTexture.IsValid())>> : std::true_type
{
};

template<typename Result, typename = void>
struct AssetBrowserThumbnailResultHasGpuTextureGeneration : std::false_type
{
};

template<typename Result>
struct AssetBrowserThumbnailResultHasGpuTextureGeneration<
    Result,
    std::void_t<decltype(std::declval<const Result&>().gpuTextureGeneration)>> : std::true_type
{
};

template<typename Result, typename = void>
struct AssetBrowserThumbnailResultHasRetainedGpuPresentation : std::false_type
{
};

template<typename Result>
struct AssetBrowserThumbnailResultHasRetainedGpuPresentation<
    Result,
    std::void_t<decltype(std::declval<const Result&>().retainedGpuPresentation)>> : std::true_type
{
};

template<typename Result>
bool AssetBrowserThumbnailResultHasDisplayImage(const Result& result)
{
    if constexpr (AssetBrowserThumbnailResultHasImagePath<Result>::value)
    {
        if (!result.imagePath.empty())
            return true;
    }
    if constexpr (AssetBrowserThumbnailResultHasRetainedImage<Result>::value)
    {
        if (result.retainedImage.has_value() &&
            !result.retainedImage->imagePath.empty())
        {
            return true;
        }
    }
    if constexpr (AssetBrowserThumbnailResultHasGpuTexture<Result>::value)
    {
        if (result.gpuTexture.IsValid())
            return true;
    }
    return false;
}

template<typename Result, typename = void>
struct AssetBrowserThumbnailResultHasPresentationKey : std::false_type
{
};

template<typename Result>
struct AssetBrowserThumbnailResultHasPresentationKey<
    Result,
    std::void_t<decltype(std::declval<const Result&>().presentationKey)>> : std::true_type
{
};

template<typename Result, typename = void>
struct AssetBrowserThumbnailResultHasRequestRevision : std::false_type
{
};

template<typename Result>
struct AssetBrowserThumbnailResultHasRequestRevision<
    Result,
    std::void_t<decltype(std::declval<const Result&>().requestRevision)>> : std::true_type
{
};

template<typename Result, typename = void>
struct AssetBrowserThumbnailResultHasPresentationState : std::false_type
{
};

template<typename Result>
struct AssetBrowserThumbnailResultHasPresentationState<
    Result,
    std::void_t<decltype(std::declval<const Result&>().presentationState)>> : std::true_type
{
};

template<typename Result>
bool AssetBrowserThumbnailResultPresentationIsPending(const Result& result)
{
    if constexpr (AssetBrowserThumbnailResultHasPresentationState<Result>::value)
    {
        using PresentationState = std::decay_t<decltype(result.presentationState)>;
        return result.presentationState == PresentationState::Loading ||
            result.presentationState == PresentationState::StaleRefreshing;
    }
    return !AssetBrowserThumbnailResultHasDisplayImage(result);
}

template<typename Result>
bool AssetBrowserThumbnailResultNeedsCanonicalRecovery(const Result& result)
{
    return AssetBrowserThumbnailResultPresentationIsPending(result) ||
        !AssetBrowserThumbnailResultHasDisplayImage(result);
}

template<typename ExistingResult, typename IncomingResult>
bool AssetBrowserThumbnailResultsSharePresentation(
    const ExistingResult& existing,
    const IncomingResult& incoming)
{
    if constexpr (
        AssetBrowserThumbnailResultHasPresentationKey<ExistingResult>::value &&
        AssetBrowserThumbnailResultHasPresentationKey<IncomingResult>::value)
    {
        return !existing.presentationKey.empty() &&
            existing.presentationKey == incoming.presentationKey;
    }
    return false;
}

template<typename ExistingResult, typename IncomingResult>
bool AssetBrowserThumbnailResultIsOlder(
    const ExistingResult& existing,
    const IncomingResult& incoming)
{
    if constexpr (
        AssetBrowserThumbnailResultHasRequestRevision<ExistingResult>::value &&
        AssetBrowserThumbnailResultHasRequestRevision<IncomingResult>::value)
    {
        const auto existingRevision = existing.requestRevision;
        const auto incomingRevision = incoming.requestRevision;
        if (existingRevision != 0u && incomingRevision != 0u)
        {
            if (incomingRevision < existingRevision)
                return true;

            if constexpr (
                AssetBrowserThumbnailResultHasPresentationKey<ExistingResult>::value &&
                AssetBrowserThumbnailResultHasPresentationKey<IncomingResult>::value)
            {
                if (incomingRevision == existingRevision &&
                    !existing.presentationKey.empty() &&
                    !incoming.presentationKey.empty() &&
                    existing.presentationKey != incoming.presentationKey)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

template<typename ExistingResult, typename IncomingResult>
bool AssetBrowserThumbnailResultCompletesSamePresentation(
    const ExistingResult& existing,
    const IncomingResult& incoming)
{
    return AssetBrowserThumbnailResultsSharePresentation(existing, incoming) &&
        AssetBrowserThumbnailResultPresentationIsPending(existing) &&
        !AssetBrowserThumbnailResultPresentationIsPending(incoming);
}

template<typename ExistingResult, typename IncomingResult>
bool AssetBrowserThumbnailResultRegressesSamePresentation(
    const ExistingResult& existing,
    const IncomingResult& incoming)
{
    return AssetBrowserThumbnailResultsSharePresentation(existing, incoming) &&
        !AssetBrowserThumbnailResultPresentationIsPending(existing) &&
        AssetBrowserThumbnailResultPresentationIsPending(incoming);
}

template<typename ExistingResult, typename IncomingResult>
std::decay_t<decltype(std::declval<const IncomingResult&>())> PreserveAssetBrowserThumbnailRetainedImage(
    const ExistingResult& existing,
    const IncomingResult& incoming)
{
    auto replacement = incoming;

    if constexpr (
        AssetBrowserThumbnailResultHasGpuTexture<ExistingResult>::value &&
        AssetBrowserThumbnailResultHasGpuTextureGeneration<ExistingResult>::value &&
        AssetBrowserThumbnailResultHasCacheEntry<ExistingResult>::value &&
        AssetBrowserThumbnailResultHasRetainedGpuPresentation<ExistingResult>::value &&
        AssetBrowserThumbnailResultHasGpuTexture<IncomingResult>::value &&
        AssetBrowserThumbnailResultHasRetainedGpuPresentation<IncomingResult>::value)
    {
        if (!replacement.gpuTexture.IsValid() &&
            !replacement.retainedGpuPresentation.has_value())
        {
            if (existing.gpuTexture.IsValid() && existing.cacheEntry.has_value())
            {
                using RetainedGpuPresentation = typename std::decay_t<
                    decltype(replacement.retainedGpuPresentation)>::value_type;
                replacement.retainedGpuPresentation = RetainedGpuPresentation {
                    existing.cacheEntry->cacheKey,
                    existing.gpuTextureGeneration
                };
            }
            else if (existing.retainedGpuPresentation.has_value())
            {
                replacement.retainedGpuPresentation = existing.retainedGpuPresentation;
            }
        }
    }

    if constexpr (
        AssetBrowserThumbnailResultHasImagePath<ExistingResult>::value &&
        AssetBrowserThumbnailResultHasRetainedImage<ExistingResult>::value &&
        AssetBrowserThumbnailResultHasImagePath<IncomingResult>::value &&
        AssetBrowserThumbnailResultHasRetainedImage<IncomingResult>::value)
    {
        if (replacement.imagePath.empty() || replacement.retainedImage.has_value())
            return replacement;

        using RetainedImage = typename std::decay_t<decltype(replacement.retainedImage)>::value_type;
        RetainedImage retained;
        if (!existing.imagePath.empty())
        {
            retained.imagePath = existing.imagePath;
            if constexpr (AssetBrowserThumbnailResultHasCacheEntry<ExistingResult>::value)
            {
                if (existing.cacheEntry.has_value())
                    retained.cacheKey = existing.cacheEntry->cacheKey;
            }
            if constexpr (AssetBrowserThumbnailResultHasRequestRevision<ExistingResult>::value)
                retained.requestRevision = existing.requestRevision;
        }
        else if (existing.retainedImage.has_value())
        {
            retained = *existing.retainedImage;
        }
        else if constexpr (AssetBrowserThumbnailResultHasCacheEntry<ExistingResult>::value)
        {
            if (existing.cacheEntry.has_value())
            {
                retained.imagePath = existing.cacheEntry->imagePath;
                retained.cacheKey = existing.cacheEntry->cacheKey;
                if constexpr (AssetBrowserThumbnailResultHasRequestRevision<ExistingResult>::value)
                    retained.requestRevision = existing.requestRevision;
            }
        }

        if (!retained.imagePath.empty() && retained.imagePath != replacement.imagePath)
        {
            if (retained.cacheKey.empty())
                retained.cacheKey = retained.imagePath.lexically_normal().generic_string();
            replacement.retainedImage = std::move(retained);
        }
    }
    return replacement;
}

template<typename ItemKeyRange, typename ResultMap, typename Result>
void ApplyAssetBrowserThumbnailResultToItemKeys(
    const ItemKeyRange& itemKeys,
    ResultMap& resultsByItemKey,
    const Result& result)
{
    for (const auto& itemKey : itemKeys)
    {
        const auto existing = resultsByItemKey.find(itemKey);
        if (existing != resultsByItemKey.end() &&
            AssetBrowserThumbnailResultRegressesSamePresentation(existing->second, result))
        {
            continue;
        }
        const bool completesSamePresentation =
            existing != resultsByItemKey.end() &&
            AssetBrowserThumbnailResultCompletesSamePresentation(existing->second, result);
        if (existing != resultsByItemKey.end() &&
            AssetBrowserThumbnailResultIsOlder(existing->second, result) &&
            !completesSamePresentation)
        {
            continue;
        }
        if (existing != resultsByItemKey.end() &&
            AssetBrowserThumbnailResultHasDisplayImage(existing->second) &&
            !AssetBrowserThumbnailResultHasDisplayImage(result) &&
            !completesSamePresentation)
        {
            continue;
        }
        resultsByItemKey[itemKey] = existing != resultsByItemKey.end()
            ? PreserveAssetBrowserThumbnailRetainedImage(existing->second, result)
            : result;
    }
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

    ApplyAssetBrowserThumbnailResultToItemKeys(
        foundItemKeys->second,
        resultsByItemKey,
        result);
}

template<typename ItemKeyMap, typename ResultMap, typename Result>
void ApplyAssetBrowserThumbnailPresentationKeyResult(
    const ItemKeyMap& itemKeysByPresentationKey,
    ResultMap& resultsByItemKey,
    const std::string& presentationKey,
    const Result& result)
{
    if (presentationKey.empty())
        return;

    const auto foundItemKeys = itemKeysByPresentationKey.find(presentationKey);
    if (foundItemKeys == itemKeysByPresentationKey.end())
        return;

    ApplyAssetBrowserThumbnailResultToItemKeys(
        foundItemKeys->second,
        resultsByItemKey,
        result);
}

struct AssetBrowserThumbnailGenerationScopeDecision
{
    bool canSkip = false;
    bool scopeChanged = false;
    bool requerySameScope = false;
};

struct AssetBrowserPostDrawThumbnailPumpPermissions
{
    bool allowGpuPreviewStart = false;
    bool allowHeavyGpuPreview = false;
};

struct AssetBrowserPostDrawThumbnailPumpInput
{
    bool interactive = false;
    bool sceneViewCameraNavigationActive = false;
    double nowSeconds = 0.0;
    double lightDeferredUntilSeconds = 0.0;
    double heavyDeferredUntilSeconds = 0.0;
};

struct AssetBrowserHeavyGpuThumbnailPumpInput
{
    bool allowHeavyGpuPreview = true;
    bool interactive = false;
    bool sceneViewCameraNavigationActive = false;
    bool hasQueuedWork = false;
    bool hasQueuedNonGpuWork = false;
    bool hasInFlightWork = false;
    bool hasQueuedReadback = false;
    bool hasQueuedResourceContinuation = false;
    bool hasPreviewRenderer = false;
    bool sceneLoadRendererResourcesPending = false;
    bool sceneLoadThumbnailEscapeHatchActive = false;
    bool hasQueuedVisibleResidentPreview = false;
    // A newer complete resident revision must not inherit the old heavy
    // preview cooldown from its provisional frame.
    bool hasQueuedReadyResidentPreview = false;
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
    bool hasExclusivePreviewContinuation = false;
    bool hasPreviewRenderer = false;
    bool standardPbrShaderPassPrewarmPending = false;
    double nowSeconds = 0.0;
    double deferredUntilSeconds = 0.0;
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
    size_t pendingTextureUploads = 0u;
    size_t interactiveStartsThisFrame = 0u;
    size_t maxInteractiveStartsPerFrame = 1u;
};

struct AssetBrowserCachedThumbnailTexturePumpDecision
{
    bool shouldPump = false;
};

struct AssetBrowserThumbnailTextureUploadBudgetInput
{
    size_t uploadedThisFrame = 0u;
    size_t maxUploadsPerFrame = 0u;
    size_t polledThisFrame = 0u;
    size_t maxPollsPerFrame = 0u;
    uint64_t elapsedMicroseconds = 0u;
    uint64_t maxElapsedMicroseconds = 0u;
};

struct AssetBrowserThumbnailRequestBudgetInput
{
    size_t requestedThisFrame = 0u;
    size_t maxRequestsPerFrame = 0u;
    uint64_t elapsedMicroseconds = 0u;
    uint64_t maxElapsedMicroseconds = 0u;
};

AssetBrowserThumbnailGenerationScopeDecision EvaluateAssetBrowserThumbnailGenerationScope(
    const std::string& previousScopeKey,
    uint32_t previousRequestedSize,
    bool scopeDirty,
    const std::string& nextScopeKey,
    uint32_t nextRequestedSize);

AssetBrowserPostDrawThumbnailPumpPermissions PlanAssetBrowserPostDrawThumbnailPump(
    const AssetBrowserPostDrawThumbnailPumpInput& input = {});

AssetBrowserHeavyGpuThumbnailPumpDecision PlanAssetBrowserHeavyGpuThumbnailPump(
    const AssetBrowserHeavyGpuThumbnailPumpInput& input);

double PlanAssetBrowserHeavyGpuThumbnailContinuationDelay(
    bool pending,
    std::string_view diagnostic,
    double resourcePendingDelaySeconds,
    double defaultDelaySeconds);

AssetBrowserLightGpuThumbnailPumpDecision PlanAssetBrowserLightGpuThumbnailPump(
    const AssetBrowserLightGpuThumbnailPumpInput& input);

AssetBrowserThumbnailPumpDecision PlanAssetBrowserThumbnailPump(
    const AssetBrowserThumbnailPumpInput& input);

AssetBrowserCachedThumbnailTexturePumpDecision PlanAssetBrowserCachedThumbnailTexturePump(
    const AssetBrowserCachedThumbnailTexturePumpInput& input);

bool ShouldContinueAssetBrowserThumbnailTextureUploads(
    const AssetBrowserThumbnailTextureUploadBudgetInput& input);

bool ShouldContinueAssetBrowserThumbnailRequests(
    const AssetBrowserThumbnailRequestBudgetInput& input);

bool ShouldRetryAssetBrowserThumbnailRequestAfterBackpressure(
    std::string_view diagnostic);

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

std::vector<std::string> SelectAssetBrowserThumbnailTextureDecodeCandidates(
    const std::vector<std::string>& queuedKeys,
    const std::function<bool(const std::string&)>& isResidentKey,
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

// Generated sub-assets can be thumbnailable even though they are not opened
// in the Asset View. Keep thumbnail scheduling independent from that UI
// capability flag so unsupported files do not consume thumbnail work.
bool CanGenerateAssetBrowserThumbnail(const AssetBrowserItem& item);

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
