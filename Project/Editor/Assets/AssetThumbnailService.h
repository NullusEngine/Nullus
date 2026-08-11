#pragma once

#include "Assets/AssetThumbnail.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetThumbnailCache.h"
#include "Assets/AssetThumbnailFeatureConfig.h"
#include "Assets/ArtifactManifest.h"

#include <atomic>
#include <chrono>
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
class ResidentPrefabPreviewRegistry;
struct EditorThumbnailPreviewResult;

struct AssetThumbnailRequestBuildContext
{
    std::unordered_map<std::string, std::optional<NLS::Core::Assets::ArtifactManifest>> artifactManifestsByAssetId;
    std::unordered_map<std::string, std::string> fileStampsByPath;
    std::unordered_map<std::string, std::filesystem::path> sourcePathsByProjectAndAssetPath;
    std::shared_ptr<const AssetDatabaseFacade> assetDatabaseSnapshot;
    std::shared_ptr<ResidentPrefabPreviewRegistry> residentPrefabPreviewRegistry;
    bool deferManifestLookups = false;
    AssetThumbnailFeatureConfig featureConfig;
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

enum class ThumbnailPresentationState
{
    Loading,
    StaleRefreshing,
    Ready,
    FailedRetained,
    Fallback
};

enum class ThumbnailPreviewQuality
{
    None,
    Canonical,
    ResidentSnapshot,
    ResidentArtifact,
    PreparedCache,
    FullArtifact
};

struct ThumbnailRetainedImage
{
    std::filesystem::path imagePath;
    std::string cacheKey;
    uint64_t requestRevision = 0u;

    [[nodiscard]] bool IsValid() const
    {
        return !imagePath.empty() && !cacheKey.empty();
    }
};

struct ThumbnailRetainedGpuPresentation
{
    std::string cacheKey;
    uint64_t textureGeneration = 0u;

    [[nodiscard]] bool IsValid() const
    {
        return !cacheKey.empty();
    }
};

enum class ThumbnailState
{
    Missing,
    Queued,
    Preparing,
    WaitingForResources,
    Rendering,
    WaitingForGpu,
    // Readback is the legacy combined readback/encode state. New ring-based
    // requests use the explicit persistence states below.
    Readback,
    Encoding,
    Persisting,
    Ready,
    Failed,
    Cancelled
};

struct AssetThumbnailServiceResult
{
    AssetThumbnailServiceStatus status = AssetThumbnailServiceStatus::Fallback;
    ThumbnailPresentationState presentationState = ThumbnailPresentationState::Fallback;
    ThumbnailPreviewQuality previewQuality = ThumbnailPreviewQuality::None;
    std::string presentationKey;
    uint64_t requestRevision = 0u;
    uint64_t requestSessionId = 0u;
    // Keeps the presentation layer from treating a resident scene preview
    // that is still waiting for resources as a terminal fallback.
    bool residentPreviewRequest = false;
    // A partial resident scene preview can be displayed immediately, but its
    // GPU texture is provisional until the source draw topology is complete.
    // Keep the registry revision so a later scene-resource attachment can
    // invalidate only that in-memory presentation without deleting a durable
    // canonical thumbnail.
    uint64_t residentPreviewRevision = 0u;
    bool residentPreviewPartial = false;
    bool refreshPending = false;
    bool failureRetained = false;
    // Request identity is copied into the result so the UI can emit one
    // actionable terminal/presentation diagnostic without reconstructing the
    // request from cache-key maps.
    std::string sourceAssetPath;
    std::string subAssetKey;
    std::string artifactPath;
    uint8_t requestKind = static_cast<uint8_t>(AssetThumbnailKind::GenericPreview);
    std::optional<AssetThumbnailCacheEntry> cacheEntry;
    std::optional<ThumbnailRetainedImage> retainedImage;
    std::optional<ThumbnailRetainedGpuPresentation> retainedGpuPresentation;
    std::filesystem::path imagePath;
    std::string fallbackIcon;
    std::string diagnostic;
    AssetThumbnailGpuTexture gpuTexture;
    uint64_t gpuTextureGeneration = 0u;
    bool revokeGpuTexture = false;
};

// Rehydrates the newest validated canonical image for a stable presentation.
// This closes UI binding races where the one-shot Fresh completion was consumed
// before an async Asset Browser scope rebuild published its item bindings.
bool PromoteAssetThumbnailResultFromPresentationIndex(
    const AssetThumbnailRequest& request,
    AssetThumbnailServiceResult& result);

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

// Builds the same source/artifact freshness fingerprint used by a canonical
// prefab thumbnail request. Scene restore uses this value when publishing a
// resident snapshot so a later Asset Browser request can only reuse an exact
// freshness match.
std::string BuildPrefabThumbnailDependencyStamp(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& sourceAssetPath,
    const std::string& subAssetKey,
    const std::string& artifactPath);

#if defined(NLS_ENABLE_TEST_HOOKS)
AssetThumbnailRequest ResolveDeferredThumbnailPreviewRequestForTesting(
    const AssetThumbnailRequest& request);

struct ThumbnailFormalLODSelectionForTesting
{
    bool loaded = false;
    uint32_t materialIndex = 0u;
    size_t vertexCount = 0u;
    size_t indexCount = 0u;
};

ThumbnailFormalLODSelectionForTesting LoadThumbnailFormalLODForTesting(
    const std::filesystem::path& path);
struct AssetThumbnailManifestLookupStatsForTesting
{
    size_t lookupCount = 0u;
    size_t mainThreadLookupCount = 0u;
    size_t backgroundThreadLookupCount = 0u;
};

void ResetAssetThumbnailManifestLookupStatsForTesting();
AssetThumbnailManifestLookupStatsForTesting GetAssetThumbnailManifestLookupStatsForTesting();
void ResetAssetThumbnailFreshnessInputCheckCountForTesting();
size_t GetAssetThumbnailFreshnessInputCheckCountForTesting();
bool ShouldRefreshGpuPreviewResourceProgressForTesting(
    uint64_t previousProgressToken,
    uint64_t progressToken,
    bool resourceWorkActive);
#endif

class AssetThumbnailService
{
public:
    explicit AssetThumbnailService(AssetThumbnailFeatureConfig featureConfig = {});
    ~AssetThumbnailService();

    // Stops background work and releases service-owned GPU/readback results.
    // Call while the rendering driver and preview renderer are still alive.
    void Shutdown();

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
    std::optional<AssetThumbnailServiceResult> ConsumeCompletedThumbnail(
        bool maintainPendingRequests = true);
    bool HasInFlightRequest() const;
    size_t GetQueuedRequestCount() const;
    ThumbnailState GetThumbnailState(const AssetThumbnailRequest& request) const;
    // Runs lifecycle cleanup that must not wait for a thumbnail render budget
    // slot, including visible resource-continuation deadlines.
    void MaintainPendingThumbnailRequests();
    void SetThumbnailGenerationBudget(ThumbnailGenerationBudget budget);
    [[nodiscard]] ThumbnailGenerationBudget GetThumbnailGenerationBudget() const;
    // Keeps the renderer's bounded resource continuation in the same adaptive
    // mode as the editor-frame scheduler. This is scheduling state only and
    // never participates in a thumbnail cache identity.
    void SetThumbnailPreviewResourcePumpBudgetMicroseconds(uint32_t budgetMicroseconds);
    void InvalidateThumbnail(const AssetThumbnailRequest& request);
    void ClearQueuedRequests();
    void SupersedeQueuedRequestsForGeneration(const std::string& generationFingerprint);
    bool HasQueuedGpuPreviewReadback() const;
    bool HasQueuedGpuPreviewResourceContinuation() const;
    bool HasQueuedGpuPreviewSceneAssemblyContinuation() const;
    [[nodiscard]] std::string GetQueuedGpuPreviewResourceContinuationSummary() const;
    bool HasQueuedVisibleResidentThumbnail() const;
    bool HasQueuedReadyResidentThumbnail();
    bool HasQueuedVisibleTextureThumbnailWork() const;
    bool HasQueuedNonGpuThumbnailWork() const;
    void SetFeatureConfig(AssetThumbnailFeatureConfig featureConfig);
    [[nodiscard]] const AssetThumbnailFeatureConfig& GetFeatureConfig() const;
    [[nodiscard]] bool IsPresentationInvalidated(const AssetThumbnailRequest& request) const;
    void PruneInvalidatedPresentationBarrier(const std::string& presentationKey);
#if defined(NLS_ENABLE_TEST_HOOKS)
    void SetGpuPreviewResourcePendingAgeForTesting(
        const AssetThumbnailRequest& request,
        std::chrono::steady_clock::duration age);
    void SetGpuPreviewResourceRequestStartAgeForTesting(
        const AssetThumbnailRequest& request,
        std::chrono::steady_clock::duration age);
    void SetVisibleThumbnailRequestAgeForTesting(
        const AssetThumbnailRequest& request,
        std::chrono::steady_clock::duration age,
        bool workStarted = true);
    void DropGpuPreviewResourcePendingOwnershipForTesting(
        const AssetThumbnailRequest& request);
    void SetThumbnailStateForTesting(
        const AssetThumbnailRequest& request,
        ThumbnailState state);
    void DropGpuPreviewResourceQueueOwnershipForTesting(
        const AssetThumbnailRequest& request);
    void DropGpuPreviewQueueLaneMembershipForTesting(
        const AssetThumbnailRequest& request);
    void QueueTerminalAndLateFreshResultForTesting(
        const AssetThumbnailRequest& request);
    [[nodiscard]] size_t GetResidentPreviewOwnerCountForTesting() const;
#endif

private:
    enum class QueuedThumbnailLane : uint8_t
    {
        Legacy,
        VisibleResident,
        Visible,
        Inspector,
        Prefetch,
        Priority,
        Background
    };

    struct InFlightThumbnailRequest
    {
        std::string cacheKey;
        uint64_t generation = 0u;
        std::shared_ptr<AssetThumbnailGenerationCancelToken> cancelToken;
        std::future<AssetThumbnailServiceResult> future;
        AssetThumbnailRequest request;
        bool requeueOnPending = false;
        bool persistenceOnly = false;
    };

    struct CoalescibleThumbnailRequest
    {
        std::string cacheKey;
        AssetThumbnailRequest request;
    };

    struct DeferredPersistenceTicket
    {
        std::string cacheKey;
        uint64_t generation = 0u;
        std::shared_ptr<AssetThumbnailGenerationCancelToken> cancelToken;
        AssetThumbnailRequest request;
        AssetThumbnailRequest metadataRequest;
        AssetThumbnailCacheEvaluation evaluation;
        std::shared_ptr<std::vector<uint8_t>> pixels;
        uint32_t width = 0u;
        uint32_t height = 0u;
    };

    struct CompletedGpuPreviewResult
    {
        uint64_t requestRevision = 0u;
        std::shared_ptr<EditorThumbnailPreviewResult> preview;
    };

    struct GpuPreviewEmptyFrameDeferral
    {
        AssetThumbnailRequest request;
        uint32_t retryCount = 0u;
        std::chrono::steady_clock::time_point lastDeferredAt {};
    };

    struct GpuPreviewResourcePendingDeferral
    {
        uint32_t retryCount = 0u;
        std::chrono::steady_clock::time_point firstDeferredAt {};
        std::chrono::steady_clock::time_point lastProgressAt {};
        uint64_t progressToken = 0u;
        bool resourceWorkActive = false;
    };

    struct GpuPreviewReadbackPendingDeferral
    {
        uint32_t pollCount = 0u;
        std::chrono::steady_clock::time_point firstPendingAt {};
    };

    struct VisibleThumbnailRequestDeadline
    {
        AssetThumbnailRequest request;
        std::chrono::steady_clock::time_point startedAt {};
        // Queue admission can legitimately take longer than generation itself
        // when a folder exposes many heavy previews. The deadline starts only
        // after this presentation receives an actual worker or renderer turn.
        bool workStarted = false;
        // A resident thumbnail may be queued before scene renderer resources
        // finish resolving. Pause its visible deadline across that scene-load
        // interval, then give the presentation layer a fresh bounded window.
        bool suspendedForSceneLoad = false;
    };

    void WaitForInFlightRequests();
    void PollCompletedGpuPreviewReadbacks(IEditorThumbnailPreviewRenderer& previewRenderer);
    void TrackGpuPreviewReadbackPending(
        const std::string& cacheKey,
        const AssetThumbnailRequest& request);
    void ClearGpuPreviewReadbackPending(const std::string& cacheKey);
    void ClearPendingQueuedRequests(bool preserveResourceDeadlines = false);
    void ClearPendingQueuedRequestsWithDiagnostics(bool preserveResourceDeadlines = false);
    size_t CountCurrentGenerationInFlightRequests() const;
    size_t CountCurrentThumbnailPreparationRequests() const;
    size_t CountCurrentVisibleTextureThumbnailPreparationRequests() const;
    size_t CountActiveThumbnailPersistenceRequests() const;
    std::optional<CoalescibleThumbnailRequest> FindCoalescibleActiveThumbnailRequest(
        const AssetThumbnailRequest& request) const;
    bool AdoptMatchingInFlightRequest(const std::string& cacheKey);
    bool StartNextThumbnailGeneration(IEditorThumbnailPreviewRenderer* previewRenderer);
    void PumpDeferredPersistenceTickets();
    bool HasQueuedCacheKeys() const;
    bool HasGpuPreviewResourceContinuation(const std::string& cacheKey) const;
    bool IsGpuPreviewResourceContinuationState(
        const std::string& cacheKey,
        ThumbnailState state) const;
    void ExpireStalledGpuPreviewResourceContinuations();
    void TrackVisibleThumbnailRequestStart(const AssetThumbnailRequest& request);
    void MarkVisibleThumbnailRequestWorkStarted(const AssetThumbnailRequest& request);
    void ClearVisibleThumbnailRequestStart(const AssetThumbnailRequest& request);
    void ExpireStalledVisibleThumbnailRequests();
    void RestoreGpuPreviewResourceContinuationRequests();
    void PromoteCompletedResidentPreviewOwners();
    bool TryPublishResidentCompletionPromotion(
        const std::string& cacheKey,
        const AssetThumbnailRequest& request);
    void ClearGpuPreviewResourcePending(const std::string& cacheKey);
    bool IsGpuPreviewEmptyFrameRetryReady(
        const GpuPreviewEmptyFrameDeferral& deferral) const;
    void RequeueReadyGpuPreviewEmptyFrameRetries();
    bool EnsureQueuedRequestCapacityFor(const std::string& cacheKey, const AssetThumbnailRequest& request);
    bool DropQueuedRequestForBackpressure(const std::string& protectedCacheKey, uint32_t maxPriorityRank);
    bool HasDeferredGpuPreviewEmptyFrame(const std::string& cacheKey) const;
    bool ObserveLatestPresentationRevision(const AssetThumbnailRequest& request);
    bool IsPresentationRevisionSuperseded(const AssetThumbnailRequest& request) const;
    void SupersedeOlderPresentationRequests(
        const std::string& presentationKey,
        uint64_t requestRevision);
    void CancelExpiredOffscreenRequests();
    void EnqueueQueuedCacheKey(
        const std::string& cacheKey,
        const AssetThumbnailRequest& request,
        bool atFront = false);
    void RepairQueuedRequestLaneMembership();
    void TrackGpuPreviewResourceRequestStart(
        const std::string& cacheKey,
        const AssetThumbnailRequest& request);
    bool SuspendResidentGpuPreviewResourceDeadlineForSceneLoad(
        const std::string& cacheKey,
        const AssetThumbnailRequest& request,
        std::chrono::steady_clock::time_point now);
    void ClearGpuPreviewResourceRequestStart(const std::string& cacheKey);
    std::optional<std::string> PopNextNonGpuThumbnailCacheKey();
    std::optional<std::string> PopNextQueuedCacheKey();
    std::optional<std::string> PopNextGpuPreviewCacheKey(
        bool includeHeavyGpuPreviews,
        bool supportsAsynchronousReadbackPolling);
    void RestoreDeferredCacheKeys(std::vector<std::string>& deferredCacheKeys);
    void RemoveQueuedCacheKeyOccurrences(const std::string& cacheKey);
    void ReleaseImportedPrefabThumbnailContinuationOwner(
        const AssetThumbnailRequest& request);

    // Resident candidates are a separate visible lane so a resource-pending
    // heavy preview cannot starve a snapshot that is already in scene memory.
    std::deque<std::string> m_queuedVisibleResidentCacheKeys;
    std::deque<std::string> m_queuedVisibleCacheKeys;
    std::deque<std::string> m_queuedInspectorCacheKeys;
    std::deque<std::string> m_queuedPrefetchCacheKeys;
    std::deque<std::string> m_queuedPriorityCacheKeys;
    std::deque<std::string> m_queuedCacheKeys;
    std::unordered_map<std::string, AssetThumbnailRequest> m_queuedRequestsByCacheKey;
    // A restarted import continuation may cold-load a complete large Prefab.
    // Keep that expensive resource phase exclusive per asset so multiple
    // pending imports cannot materialize their full scenes at the same time.
    // Asset identity, rather than cache identity, keeps size/revision aliases
    // on the same owner while the continuation is active.
    std::string m_activeImportedPrefabThumbnailContinuationAssetId;
    // A completed full frame may leave older size/freshness aliases queued.
    // They can reuse resident resources, but must yield to an imported asset
    // that has not produced a frame in this generation yet.
    std::unordered_set<std::string>
        m_completedImportedPrefabThumbnailContinuationAssetIds;
    // Each import registration may bypass an older recoverable negative cache
    // entry once. Recording the registration revision prevents a timeout from
    // immediately requeueing itself while still allowing a later reimport.
    std::unordered_map<std::string, uint64_t>
        m_importedPrefabThumbnailAttemptRevisionByCacheKey;
    // The request map owns request data; this index owns queue membership.
    // Normal lane moves remove only the indexed deque. A missing index is
    // treated as a recoverable bookkeeping gap by RepairQueuedRequestLaneMembership.
    std::unordered_map<std::string, QueuedThumbnailLane> m_queuedThumbnailLaneByCacheKey;
    std::unordered_map<std::string, AssetThumbnailRequest> m_resolvedPreviewRequestsByCacheKey;
    // Queue/pending/resolved tables are generation-scoped and may all be
    // rebuilt during a folder or scene transition. Keep the latest resident
    // request identity separately so a scene package that completes after that
    // transition still has a request to promote and render.
    std::unordered_map<std::string, AssetThumbnailRequest>
        m_residentPreviewRequestsByPresentationKey;
    std::unordered_map<std::string, std::future<AssetThumbnailRequest>> m_previewRequestResolutionFuturesByCacheKey;
    std::unordered_map<std::string, AssetThumbnailServiceResult> m_stableThumbnailResultsByCacheKey;
    // Resource continuations can time out from a scheduler/maintenance pass
    // without a generation future completing. Keep that terminal result until
    // the UI consumes it, otherwise the tile remains visibly Pending forever.
    std::unordered_map<std::string, AssetThumbnailServiceResult>
        m_terminalThumbnailResultsByCacheKey;
    // A terminal result must also fence late preparation/readback workers. The
    // cache-key map can change after deferred artifact resolution, so this
    // barrier is indexed by the stable presentation identity.
    std::unordered_map<std::string, uint64_t> m_terminalPresentationRevisions;
    std::unordered_set<std::string> m_gpuDeferredHeavyPreviewCacheKeys;
    std::unordered_set<std::string> m_gpuPreviewEmptyFrameDeferredCacheKeys;
    // A detailed, truncated resource report has not finished enumerating its
    // dependencies. Let one scheduler call pass before pumping it again so a
    // single frame cannot repeatedly spend its preview budget on the same key.
    std::unordered_set<std::string> m_gpuPreviewResourcePendingDeferredCacheKeys;
    // Resource readiness is transient, but it must not leave a visible request
    // pending forever when a dependency or proxy build cannot make progress.
    // Keep the original request separately from the resolved preview request so
    // a bookkeeping race cannot strand a WaitingForResources state without a
    // queue entry that the scheduler can restore.
    std::unordered_map<std::string, AssetThumbnailRequest>
        m_gpuPreviewResourcePendingRequestsByCacheKey;
    // A partial resident frame remains displayable while the scene continues
    // resolving resources. Hold its registry revision so the continuation is
    // requeued only after the scene publishes a newer package.
    std::unordered_map<std::string, uint64_t>
        m_gpuPreviewResidentPartialRevisionByCacheKey;
    // The preview renderer owns one transient prefab scene. Once time-sliced
    // scene assembly starts, keep that request on the heavy continuation lane
    // until its proxy list is complete so another preview cannot reset it.
    std::string m_gpuPreviewSceneAssemblyContinuationCacheKey;
    // Completion is a one-shot promotion event. Keep it separate from the
    // resource-pending table so a queue/lane repair cannot lose the immediate
    // canonical render opportunity after a partial resident frame.
    std::unordered_set<std::string> m_gpuPreviewReadyResidentCacheKeys;
    // A ready marker is consumed when submission starts. Remember which
    // registry revision received that one-shot promotion so maintenance does
    // not recreate the marker on every resource-continuation pump.
    std::unordered_map<std::string, uint64_t>
        m_promotedResidentRevisionByPresentationKey;
    std::unordered_map<std::string, GpuPreviewResourcePendingDeferral>
        m_gpuPreviewResourcePendingDeferralsByCacheKey;
    // Start the resource deadline when a visible GPU preview is first queued,
    // rather than when lane scheduling happens to inspect its dependencies.
    // Keep a second stable index because the cache key changes when a deferred
    // manifest lookup supplies the physical artifact path.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        m_gpuPreviewResourceRequestStartedAtByCacheKey;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        m_gpuPreviewResourceRequestStartedAtByPresentationKey;
    std::unordered_map<std::string, std::string>
        m_gpuPreviewResourcePresentationKeyByCacheKey;
    std::vector<std::string>
        m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad;
    std::unordered_map<std::string, GpuPreviewEmptyFrameDeferral> m_gpuPreviewEmptyFrameDeferralsByCacheKey;
    std::unordered_set<std::string> m_gpuPreviewReadbackPendingCacheKeys;
    std::unordered_map<std::string, AssetThumbnailRequest> m_gpuPreviewReadbackPendingRequestsByCacheKey;
    std::unordered_map<std::string, GpuPreviewReadbackPendingDeferral>
        m_gpuPreviewReadbackPendingDeferralsByCacheKey;
    // Readback tickets carry the renderer request identity. Keep a reverse
    // index so completion polling does not scan every pending cache key.
    std::unordered_map<std::string, std::string> m_gpuPreviewReadbackCacheKeyByRequestKey;
    std::unordered_map<std::string, ThumbnailState> m_thumbnailStatesByCacheKey;
    // Visible requests use the same stable identity across queue, preparation,
    // resource continuation, and resolved-artifact cache-key transitions.
    std::unordered_map<std::string, VisibleThumbnailRequestDeadline>
        m_visibleThumbnailRequestDeadlinesByPresentationKey;
    // Presentation-level deletion barriers prevent an older in-flight result
    // from being surfaced again after its asset was removed.
    std::unordered_map<std::string, uint64_t> m_invalidatedPresentationRevisions;
    // A stable presentation may receive a newer freshness revision while an
    // older preparation or readback is still in flight. Keep the newest
    // revision in memory so late results cannot be surfaced as the current UI
    // image. Submitted GPU work is retired through the normal renderer path.
    std::unordered_map<std::string, uint64_t> m_latestPresentationRevisions;
    std::string m_generationFingerprint;
    uint64_t m_generationSerial = 0u;
    uint64_t m_requestSessionId = 0u;
    uint64_t m_nextRequestRevision = 1u;
    std::shared_ptr<AssetThumbnailGenerationCancelToken> m_generationCancelToken;
    std::vector<InFlightThumbnailRequest> m_inFlightThumbnails;
    std::deque<DeferredPersistenceTicket> m_deferredPersistenceTickets;
    std::unordered_map<
        std::string,
        CompletedGpuPreviewResult> m_completedGpuPreviewResultsByCacheKey;
    ThumbnailGenerationBudget m_generationBudget;
    uint32_t m_thumbnailPreviewResourcePumpBudgetMicroseconds = 1000u;
    bool m_hasExplicitGenerationBudget = false;
    bool m_shutdown = false;
    size_t m_priorityThumbnailDequeueStreak = 0u;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_offscreenSinceByCacheKey;
    std::string m_lastMaintenanceTelemetrySignature;
    std::chrono::steady_clock::time_point m_lastMaintenanceTelemetryAt {};
    AssetThumbnailFeatureConfig m_featureConfig;
};
}
