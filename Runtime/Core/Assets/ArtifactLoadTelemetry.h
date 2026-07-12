#pragma once

#include "CoreDef.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace NLS::Core::Assets
{
enum class ArtifactLoadTelemetryStage : uint8_t
{
    PrefabGraphLoad,
    ManifestValidation,
    DependencyScan,
    NativeArtifactFileRead,
    NativeContainerParseHash,
    NativeArtifactLowCopyView,
    NativeArtifactPayloadCopy,
    CpuDeserialize,
    RuntimeResourceCreation,
    GpuUpload,
    CacheHit,
    CacheMiss,
    Cancellation,
    LifetimeAcquire,
    LifetimeRelease,
    LifetimeTrimSkip,
    Eviction,
    PrefabVisiblePrewarmSchedule,
    PrefabVisiblePrewarmLoad,
    PrefabUnifiedSharedLoad,
    PrefabRendererTaskBuild,
    PrefabRendererResolutionStep,
    ThumbnailGpuPreviewRender,
    ThumbnailGpuPreviewPrepareResources,
    ThumbnailGpuPreviewPumpDependencies,
    ThumbnailGpuPreviewPumpMeshDependencies,
    ThumbnailGpuPreviewPumpMaterialDependencies,
    ThumbnailGpuPreviewPumpTextureDependencies,
    ThumbnailGpuPreviewPumpMaterialPathBuild,
    ThumbnailGpuPreviewPumpMaterialPromote,
    ThumbnailGpuPreviewPumpMaterialReadyScan,
    ThumbnailGpuPreviewPumpMaterialFutureGet,
    ThumbnailGpuPreviewPumpMaterialRuntimeCreate,
    ThumbnailGpuPreviewPumpMaterialShaderPassResolve,
    ThumbnailGpuPreviewPumpMaterialShaderPassLoad,
    ThumbnailGpuPreviewBackgroundMaterialShaderPassLoad,
    ThumbnailGpuPreviewPumpMaterialRegister,
    ThumbnailGpuPreviewRecord,
    ThumbnailGpuPreviewSubmit,
    ThumbnailGpuPreviewDrain,
    ThumbnailGpuPreviewCleanup,
    ThumbnailGpuPreviewReadback,
    ThumbnailGpuPreviewPollReadback,
    ThumbnailTextureDecode,
    ThumbnailTextureUploadEnqueue,
    ThumbnailTextureUpload,
    ThumbnailTextureUploadPreparePixels,
    ThumbnailTextureUploadCreate,
    ThumbnailTextureUploadCreateView,
    ThumbnailTextureUploadSubmit,
    ThumbnailTextureUploadPublish,
    ThumbnailTextureUploadResolveUiId,
    ThumbnailUiDraw,
    ThumbnailUiDrawGridVisibleRows,
    ThumbnailUiDrawGridItemInteractions,
    ThumbnailUiDrawGridItemThumbnail,
    ThumbnailUiDrawGridItemLabel,
    ThumbnailUiDrawVisibleSet,
    ThumbnailUiDrawVisibleSetHash,
    ThumbnailUiDrawVisibleSetApply,
    ThumbnailUiDrawVisibleSetHotCacheFlush,
    ThumbnailUiDrawGenerationScope,
    ThumbnailUiDrawGenerationScopeSelectItems,
    ThumbnailUiDrawGenerationScopeBuildKey,
    ThumbnailUiDrawGenerationScopeItemKey,
    ThumbnailUiDrawGenerationScopeResultLookup,
    ThumbnailUiDrawGenerationScopeBuildRequest,
    ThumbnailUiDrawGenerationScopeBuildRequestValidate,
    ThumbnailUiDrawGenerationScopeBuildRequestMetaId,
    ThumbnailUiDrawGenerationScopeBuildRequestManifestLookup,
    ThumbnailUiDrawGenerationScopeBuildRequestItemIdentity,
    ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshness,
    ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessResolve,
    ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessFileStamp,
    ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessMetaStamp,
    ThumbnailUiDrawGenerationScopeBuildRequestArtifactFreshness,
    ThumbnailUiDrawGenerationScopeBuildRequestDependencyStamp,
    ThumbnailUiDrawGenerationScopeRequestPreview,
    ThumbnailServiceRequestStableLookup,
    ThumbnailServiceRequestCacheEvaluate,
    ThumbnailServiceRequestQueue,
    ThumbnailServiceGpuPreviewQueueDecision,
    ThumbnailTexturePump,
    ThumbnailTexturePumpConsumeCompleted,
    ThumbnailTexturePumpPendingUploadPoll,
    ThumbnailTexturePumpPendingUploadConsumeResult,
    ThumbnailTexturePumpPendingUploadResolveUiId,
    ThumbnailTexturePumpPendingUploadWrapTexture,
    ThumbnailTexturePumpPendingUploadCachePublish,
    ThumbnailTexturePumpReadyDecodePoll,
    ThumbnailTexturePumpReadyDecodeLoad,
    ThumbnailTexturePumpStartDecodes,
    ThumbnailTexturePumpBuildResidentSet,
    ThumbnailTexturePumpSelectDecodeCandidates,
    ThumbnailTexturePumpScheduleDecodeJobs,
    ThumbnailTextureUploadDeferred,
    ThumbnailUiPostDrawPump,
    ThumbnailGpuPreviewPrepareMaterialResources,
    ThumbnailGpuPreviewPrepareSceneObjects,
    ThumbnailUiPostDrawPumpConsumeCompleted,
    ThumbnailUiPostDrawPumpCreatePreviewRenderer,
    ThumbnailUiPostDrawPumpStartLightGpu,
    ThumbnailUiPostDrawPumpStartHeavyGpu,
    ThumbnailUiPostDrawPumpStartBackground,
    ThumbnailCacheEvaluateResolveEntry,
    ThumbnailCacheEvaluateResolveEntryBuild,
    ThumbnailCacheEvaluateResolveEntryContainmentKey,
    ThumbnailCacheEvaluateResolveEntryContainmentStamp,
    ThumbnailCacheEvaluateResolveEntryContainmentValidate,
    ThumbnailCacheEvaluateMetadataStat,
    ThumbnailCacheEvaluateMetadataLoad,
    ThumbnailCacheEvaluateFreshness,
    ThumbnailCacheEvaluateImageStat,
    ThumbnailCacheEvaluateImageValidate
};

enum class ArtifactLoadBudgetKind : uint8_t
{
    WarmTexturedPreviewFirstVisible,
    MouseReleaseUiThreadWork,
    HotCacheLookup,
    PrefabVisiblePrewarm,
    PrefabUnifiedSharedLoad,
    PrefabRendererTaskBuild,
    PrefabRendererResolutionStep
};

struct ArtifactLoadTelemetryRecord
{
    ArtifactLoadTelemetryStage stage = ArtifactLoadTelemetryStage::CacheMiss;
    std::chrono::microseconds elapsed {};
    size_t byteCount = 0u;
    std::string path;
};

struct ArtifactLoadBudgetMissRecord
{
    ArtifactLoadBudgetKind kind = ArtifactLoadBudgetKind::WarmTexturedPreviewFirstVisible;
    std::chrono::milliseconds elapsed {};
    int budgetMs = 0;
    std::string path;
};

struct ArtifactLoadTelemetryStageSummary
{
    ArtifactLoadTelemetryStage stage = ArtifactLoadTelemetryStage::CacheMiss;
    std::string path;
    size_t recordCount = 0u;
    std::chrono::microseconds totalElapsed {};
    size_t totalBytes = 0u;
};

struct ArtifactLoadBudget
{
    static constexpr int kWarmTexturedPreviewFirstVisibleBudgetMs = 200;
    static constexpr int kMouseReleaseFrameBudgetMs = 17;
    static constexpr int kHotCacheLookupBudgetMs = 10;
    static constexpr int kPrefabVisiblePrewarmBudgetMs = 250;
    static constexpr int kPrefabUnifiedSharedLoadBudgetMs = 50;
    static constexpr int kPrefabRendererTaskBuildBudgetMs = 5;
    static constexpr int kPrefabRendererResolutionStepBudgetMs = 5;
};

namespace Detail
{
#if !defined(NDEBUG) || defined(NLS_ENABLE_TEST_HOOKS) || defined(NLS_ENABLE_ARTIFACT_LOAD_TELEMETRY)
inline constexpr bool kArtifactLoadTelemetryEnabledByDefault = true;
#else
inline constexpr bool kArtifactLoadTelemetryEnabledByDefault = false;
#endif

inline constexpr size_t kMaxArtifactLoadTelemetryRecords = 16384u;
inline constexpr size_t kMaxArtifactLoadBudgetMissRecords = 256u;
}

NLS_CORE_API void SetArtifactLoadTelemetryEnabled(bool enabled);
NLS_CORE_API bool IsArtifactLoadTelemetryEnabled();
NLS_CORE_API void RecordArtifactLoadTelemetry(const ArtifactLoadTelemetryRecord& record);
NLS_CORE_API std::vector<ArtifactLoadTelemetryRecord> SnapshotArtifactLoadTelemetry();
NLS_CORE_API std::vector<ArtifactLoadTelemetryStageSummary> SummarizeArtifactLoadTelemetry();
NLS_CORE_API std::vector<ArtifactLoadBudgetMissRecord> SnapshotArtifactLoadBudgetMisses();
NLS_CORE_API void ClearArtifactLoadTelemetry();
NLS_CORE_API const char* ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage stage);

inline bool BudgetExceeded(const std::chrono::milliseconds elapsed, const int budgetMs)
{
    return elapsed.count() > budgetMs;
}

inline int BudgetMilliseconds(const ArtifactLoadBudgetKind kind)
{
    switch (kind)
    {
    case ArtifactLoadBudgetKind::WarmTexturedPreviewFirstVisible:
        return ArtifactLoadBudget::kWarmTexturedPreviewFirstVisibleBudgetMs;
    case ArtifactLoadBudgetKind::MouseReleaseUiThreadWork:
        return ArtifactLoadBudget::kMouseReleaseFrameBudgetMs;
    case ArtifactLoadBudgetKind::HotCacheLookup:
        return ArtifactLoadBudget::kHotCacheLookupBudgetMs;
    case ArtifactLoadBudgetKind::PrefabVisiblePrewarm:
        return ArtifactLoadBudget::kPrefabVisiblePrewarmBudgetMs;
    case ArtifactLoadBudgetKind::PrefabUnifiedSharedLoad:
        return ArtifactLoadBudget::kPrefabUnifiedSharedLoadBudgetMs;
    case ArtifactLoadBudgetKind::PrefabRendererTaskBuild:
        return ArtifactLoadBudget::kPrefabRendererTaskBuildBudgetMs;
    case ArtifactLoadBudgetKind::PrefabRendererResolutionStep:
        return ArtifactLoadBudget::kPrefabRendererResolutionStepBudgetMs;
    }
    return ArtifactLoadBudget::kWarmTexturedPreviewFirstVisibleBudgetMs;
}

NLS_CORE_API void RecordArtifactLoadBudgetMiss(const ArtifactLoadBudgetMissRecord& record);

inline bool CheckArtifactLoadBudget(const std::chrono::milliseconds elapsed, const int budgetMs)
{
    return !BudgetExceeded(elapsed, budgetMs);
}

inline bool CheckArtifactLoadBudget(
    const ArtifactLoadBudgetKind kind,
    const std::chrono::milliseconds elapsed,
    std::string path = {})
{
    const int budgetMs = BudgetMilliseconds(kind);
    if (!BudgetExceeded(elapsed, budgetMs))
        return true;

    ArtifactLoadBudgetMissRecord record;
    record.kind = kind;
    record.elapsed = elapsed;
    record.budgetMs = budgetMs;
    record.path = std::move(path);
    RecordArtifactLoadBudgetMiss(record);
    return false;
}
}
