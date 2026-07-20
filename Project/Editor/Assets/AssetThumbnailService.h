#pragma once

#include "Assets/AssetThumbnail.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetThumbnailCache.h"
#include "Assets/ArtifactManifest.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <future>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Editor::Assets
{
class EditorThumbnailPreviewRenderer;
class IEditorThumbnailPreviewRenderer;

struct AssetThumbnailRequestBuildContext
{
    std::unordered_map<std::string, std::optional<NLS::Core::Assets::ArtifactManifest>> artifactManifestsByAssetId;
    std::unordered_map<std::string, std::string> fileStampsByPath;
    std::unordered_map<std::string, std::filesystem::path> sourcePathsByProjectAndAssetPath;
    std::filesystem::path artifactDatabaseProjectRoot;
    std::filesystem::path artifactDatabasePath;
    std::string artifactDatabaseStamp;
    bool artifactDatabaseStampCached = false;
    bool deferManifestLookups = false;
};

struct AssetThumbnailGenerationCancelToken
{
    std::atomic_bool cancelled {false};
    uint64_t generation = 0u;
};

enum class AssetThumbnailServiceStatus
{
    Fresh,
    Pending,
    Fallback,
    Failed
};

enum class ThumbnailState
{
    Missing,
    Queued,
    Preparing,
    WaitingForResources,
    Rendering,
    WaitingForGpu,
    Readback,
    Ready,
    Failed,
    Cancelled
};

struct AssetThumbnailServiceResult
{
    AssetThumbnailServiceStatus status = AssetThumbnailServiceStatus::Fallback;
    std::optional<AssetThumbnailCacheEntry> cacheEntry;
    std::filesystem::path imagePath;
    std::string fallbackIcon;
    std::string diagnostic;
    AssetThumbnailGpuTexture gpuTexture;
    uint64_t gpuTextureGeneration = 0u;
};

struct ThumbnailGenerationBudget
{
    size_t previewRenderCountBudget = 1u;
    size_t readbackCountBudget = 1u;
    size_t cacheWriteCountBudget = 1u;
    size_t cpuPreparationByteBudget = SIZE_MAX;
    size_t gpuUploadByteBudget = SIZE_MAX;
};

std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItem(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    uint32_t requestedSize);
std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItem(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    uint32_t requestedSize,
    AssetThumbnailRequestBuildContext& context);

#if defined(NLS_ENABLE_TEST_HOOKS)
struct ThumbnailFormalLODSelectionForTesting
{
    bool loaded = false;
    uint32_t materialIndex = 0u;
    size_t vertexCount = 0u;
    size_t indexCount = 0u;
};

ThumbnailFormalLODSelectionForTesting LoadThumbnailFormalLODForTesting(
    const std::filesystem::path& path);
#endif

class AssetThumbnailService
{
public:
    ~AssetThumbnailService();

    AssetThumbnailServiceResult RequestAssetPreview(const AssetThumbnailRequest& request);
    AssetThumbnailServiceResult GetAssetPreview(const AssetThumbnailRequest& request);
    AssetThumbnailServiceResult GetMiniThumbnail(const AssetThumbnailRequest& request) const;
    bool IsLoadingAssetPreview(const AssetThumbnailRequest& request) const;

    AssetThumbnailServiceResult GetThumbnail(const AssetThumbnailRequest& request);
    std::optional<AssetThumbnailServiceResult> GenerateNextThumbnail();
    std::optional<AssetThumbnailServiceResult> GenerateNextThumbnail(
        EditorThumbnailPreviewRenderer& previewRenderer,
        bool includeHeavyGpuPreviews = true);
    std::optional<AssetThumbnailServiceResult> GenerateNextThumbnail(
        IEditorThumbnailPreviewRenderer& previewRenderer,
        bool includeHeavyGpuPreviews = true);
    bool StartNextThumbnailGeneration();
    bool StartNextThumbnailGeneration(EditorThumbnailPreviewRenderer& previewRenderer);
    bool StartNextThumbnailGeneration(IEditorThumbnailPreviewRenderer& previewRenderer);
    std::optional<AssetThumbnailServiceResult> ConsumeCompletedThumbnail();
    bool HasInFlightRequest() const;
    size_t GetQueuedRequestCount() const;
    ThumbnailState GetThumbnailState(const AssetThumbnailRequest& request) const;
    void SetThumbnailGenerationBudget(ThumbnailGenerationBudget budget);
    [[nodiscard]] ThumbnailGenerationBudget GetThumbnailGenerationBudget() const;
    void ClearQueuedRequests();
    void SupersedeQueuedRequestsForGeneration(const std::string& generationFingerprint);
    bool HasQueuedGpuPreviewReadback() const;
    bool HasQueuedGpuPreviewResourceContinuation() const;

private:
    struct InFlightThumbnailRequest
    {
        std::string cacheKey;
        uint64_t generation = 0u;
        std::shared_ptr<AssetThumbnailGenerationCancelToken> cancelToken;
        std::future<AssetThumbnailServiceResult> future;
        AssetThumbnailRequest request;
        bool requeueOnPending = false;
    };

    void WaitForInFlightRequests();
    void ClearPendingQueuedRequests();
    void ClearPendingQueuedRequestsWithDiagnostics();
    size_t CountCurrentGenerationInFlightRequests() const;
    bool AdoptMatchingInFlightRequest(const std::string& cacheKey);
    bool StartNextThumbnailGeneration(IEditorThumbnailPreviewRenderer* previewRenderer);
    bool HasQueuedCacheKeys() const;
    bool EnsureQueuedRequestCapacityFor(const std::string& cacheKey, const AssetThumbnailRequest& request);
    bool DropQueuedRequestForBackpressure(const std::string& protectedCacheKey, uint32_t maxPriorityRank);
    bool HasDeferredGpuPreviewEmptyFrame(const std::string& cacheKey) const;
    void EnqueueQueuedCacheKey(
        const std::string& cacheKey,
        const AssetThumbnailRequest& request,
        bool atFront = false);
    std::optional<std::string> PopNextQueuedCacheKey();
    std::optional<std::string> PopNextGpuPreviewCacheKey(bool includeHeavyGpuPreviews);
    void RestoreDeferredCacheKeys(std::vector<std::string>& deferredCacheKeys);
    void RemoveQueuedCacheKeyOccurrences(const std::string& cacheKey);

    std::deque<std::string> m_queuedVisibleCacheKeys;
    std::deque<std::string> m_queuedInspectorCacheKeys;
    std::deque<std::string> m_queuedPrefetchCacheKeys;
    std::deque<std::string> m_queuedPriorityCacheKeys;
    std::deque<std::string> m_queuedCacheKeys;
    std::unordered_map<std::string, AssetThumbnailRequest> m_queuedRequestsByCacheKey;
    std::unordered_map<std::string, AssetThumbnailRequest> m_resolvedPreviewRequestsByCacheKey;
    std::unordered_map<std::string, AssetThumbnailServiceResult> m_stableThumbnailResultsByCacheKey;
    std::unordered_set<std::string> m_gpuDeferredHeavyPreviewCacheKeys;
    std::unordered_set<std::string> m_gpuPreviewEmptyFrameDeferredCacheKeys;
    std::unordered_set<std::string> m_gpuPreviewResourcePendingDeferredCacheKeys;
    std::unordered_set<std::string> m_gpuPreviewReadbackPendingCacheKeys;
    std::unordered_map<std::string, AssetThumbnailRequest> m_gpuPreviewReadbackPendingRequestsByCacheKey;
    std::unordered_map<std::string, ThumbnailState> m_thumbnailStatesByCacheKey;
    std::string m_generationFingerprint;
    uint64_t m_generationSerial = 0u;
    std::shared_ptr<AssetThumbnailGenerationCancelToken> m_generationCancelToken;
    std::vector<InFlightThumbnailRequest> m_inFlightThumbnails;
    ThumbnailGenerationBudget m_generationBudget;
    bool m_hasExplicitGenerationBudget = false;
    size_t m_priorityThumbnailDequeueStreak = 0u;
};
}
