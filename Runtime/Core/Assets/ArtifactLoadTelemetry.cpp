#include "Assets/ArtifactLoadTelemetry.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace NLS::Core::Assets
{
namespace
{
std::mutex& ArtifactLoadTelemetryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<ArtifactLoadTelemetryRecord>& ArtifactLoadTelemetryRecords()
{
    static std::vector<ArtifactLoadTelemetryRecord> records;
    return records;
}

std::vector<ArtifactLoadBudgetMissRecord>& ArtifactLoadBudgetMissRecords()
{
    static std::vector<ArtifactLoadBudgetMissRecord> records;
    return records;
}

std::atomic_bool& ArtifactLoadTelemetryEnabledFlag()
{
    static std::atomic_bool enabled { Detail::kArtifactLoadTelemetryEnabledByDefault };
    return enabled;
}

template <typename T>
void PushBounded(std::vector<T>& records, const T& record, const size_t maxRecords)
{
    if (records.size() >= maxRecords)
        records.erase(records.begin());
    records.push_back(record);
}
}

void SetArtifactLoadTelemetryEnabled(const bool enabled)
{
    ArtifactLoadTelemetryEnabledFlag().store(enabled, std::memory_order_relaxed);
}

bool IsArtifactLoadTelemetryEnabled()
{
    return ArtifactLoadTelemetryEnabledFlag().load(std::memory_order_relaxed);
}

void RecordArtifactLoadTelemetry(const ArtifactLoadTelemetryRecord& record)
{
    if (!IsArtifactLoadTelemetryEnabled())
        return;

    std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
    PushBounded(
        ArtifactLoadTelemetryRecords(),
        record,
        Detail::kMaxArtifactLoadTelemetryRecords);
}

std::vector<ArtifactLoadTelemetryRecord> SnapshotArtifactLoadTelemetry()
{
    if (!IsArtifactLoadTelemetryEnabled())
        return {};

    std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
    return ArtifactLoadTelemetryRecords();
}

std::vector<ArtifactLoadTelemetryStageSummary> SummarizeArtifactLoadTelemetry()
{
    if (!IsArtifactLoadTelemetryEnabled())
        return {};

    std::vector<ArtifactLoadTelemetryRecord> records;
    {
        std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
        records = ArtifactLoadTelemetryRecords();
    }

    std::vector<ArtifactLoadTelemetryStageSummary> summaries;
    for (const auto& record : records)
    {
        auto found = std::find_if(
            summaries.begin(),
            summaries.end(),
            [&record](const ArtifactLoadTelemetryStageSummary& summary)
            {
                return summary.stage == record.stage && summary.path == record.path;
            });
        if (found == summaries.end())
        {
            summaries.push_back({
                record.stage,
                record.path,
                1u,
                record.elapsed,
                record.byteCount });
            continue;
        }

        ++found->recordCount;
        found->totalElapsed += record.elapsed;
        found->totalBytes += record.byteCount;
    }
    return summaries;
}

std::vector<ArtifactLoadBudgetMissRecord> SnapshotArtifactLoadBudgetMisses()
{
    if (!IsArtifactLoadTelemetryEnabled())
        return {};

    std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
    return ArtifactLoadBudgetMissRecords();
}

void ClearArtifactLoadTelemetry()
{
    std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
    ArtifactLoadTelemetryRecords().clear();
    ArtifactLoadBudgetMissRecords().clear();
}

void RecordArtifactLoadBudgetMiss(const ArtifactLoadBudgetMissRecord& record)
{
    if (!IsArtifactLoadTelemetryEnabled())
        return;

    std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
    PushBounded(
        ArtifactLoadBudgetMissRecords(),
        record,
        Detail::kMaxArtifactLoadBudgetMissRecords);
}

const char* ArtifactLoadTelemetryStageName(const ArtifactLoadTelemetryStage stage)
{
    switch (stage)
    {
    case ArtifactLoadTelemetryStage::PrefabGraphLoad:
        return "PrefabGraphLoad";
    case ArtifactLoadTelemetryStage::ManifestValidation:
        return "ManifestValidation";
    case ArtifactLoadTelemetryStage::DependencyScan:
        return "DependencyScan";
    case ArtifactLoadTelemetryStage::NativeArtifactFileRead:
        return "NativeArtifactFileRead";
    case ArtifactLoadTelemetryStage::NativeContainerParseHash:
        return "NativeContainerParseHash";
    case ArtifactLoadTelemetryStage::NativeArtifactLowCopyView:
        return "NativeArtifactLowCopyView";
    case ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy:
        return "NativeArtifactPayloadCopy";
    case ArtifactLoadTelemetryStage::CpuDeserialize:
        return "CpuDeserialize";
    case ArtifactLoadTelemetryStage::RuntimeResourceCreation:
        return "RuntimeResourceCreation";
    case ArtifactLoadTelemetryStage::GpuUpload:
        return "GpuUpload";
    case ArtifactLoadTelemetryStage::GpuResourceCreate:
        return "GpuResourceCreate";
    case ArtifactLoadTelemetryStage::GpuUploadPlanBuild:
        return "GpuUploadPlanBuild";
    case ArtifactLoadTelemetryStage::GpuUploadPrepare:
        return "GpuUploadPrepare";
    case ArtifactLoadTelemetryStage::GpuUploadCpuCopy:
        return "GpuUploadCpuCopy";
    case ArtifactLoadTelemetryStage::GpuUploadCommandSetup:
        return "GpuUploadCommandSetup";
    case ArtifactLoadTelemetryStage::GpuUploadSubmit:
        return "GpuUploadSubmit";
    case ArtifactLoadTelemetryStage::GpuUploadFenceWait:
        return "GpuUploadFenceWait";
    case ArtifactLoadTelemetryStage::NativeArtifactFileMap:
        return "NativeArtifactFileMap";
    case ArtifactLoadTelemetryStage::CacheHit:
        return "CacheHit";
    case ArtifactLoadTelemetryStage::CacheMiss:
        return "CacheMiss";
    case ArtifactLoadTelemetryStage::Cancellation:
        return "Cancellation";
    case ArtifactLoadTelemetryStage::LifetimeAcquire:
        return "LifetimeAcquire";
    case ArtifactLoadTelemetryStage::LifetimeRelease:
        return "LifetimeRelease";
    case ArtifactLoadTelemetryStage::LifetimeTrimSkip:
        return "LifetimeTrimSkip";
    case ArtifactLoadTelemetryStage::Eviction:
        return "Eviction";
    case ArtifactLoadTelemetryStage::PrefabVisiblePrewarmSchedule:
        return "PrefabVisiblePrewarmSchedule";
    case ArtifactLoadTelemetryStage::PrefabVisiblePrewarmLoad:
        return "PrefabVisiblePrewarmLoad";
    case ArtifactLoadTelemetryStage::PrefabUnifiedSharedLoad:
        return "PrefabUnifiedSharedLoad";
    case ArtifactLoadTelemetryStage::PrefabRendererTaskBuild:
        return "PrefabRendererTaskBuild";
    case ArtifactLoadTelemetryStage::PrefabRendererResolutionStep:
        return "PrefabRendererResolutionStep";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender:
        return "ThumbnailGpuPreviewRender";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources:
        return "ThumbnailGpuPreviewPrepareResources";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpDependencies:
        return "ThumbnailGpuPreviewPumpDependencies";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMeshDependencies:
        return "ThumbnailGpuPreviewPumpMeshDependencies";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialDependencies:
        return "ThumbnailGpuPreviewPumpMaterialDependencies";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpTextureDependencies:
        return "ThumbnailGpuPreviewPumpTextureDependencies";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialPathBuild:
        return "ThumbnailGpuPreviewPumpMaterialPathBuild";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialPromote:
        return "ThumbnailGpuPreviewPumpMaterialPromote";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialReadyScan:
        return "ThumbnailGpuPreviewPumpMaterialReadyScan";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialFutureGet:
        return "ThumbnailGpuPreviewPumpMaterialFutureGet";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialRuntimeCreate:
        return "ThumbnailGpuPreviewPumpMaterialRuntimeCreate";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialShaderPassResolve:
        return "ThumbnailGpuPreviewPumpMaterialShaderPassResolve";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialShaderPassLoad:
        return "ThumbnailGpuPreviewPumpMaterialShaderPassLoad";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewBackgroundMaterialShaderPassLoad:
        return "ThumbnailGpuPreviewBackgroundMaterialShaderPassLoad";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialRegister:
        return "ThumbnailGpuPreviewPumpMaterialRegister";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRecord:
        return "ThumbnailGpuPreviewRecord";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewSubmit:
        return "ThumbnailGpuPreviewSubmit";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewDrain:
        return "ThumbnailGpuPreviewDrain";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewCleanup:
        return "ThumbnailGpuPreviewCleanup";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewReadback:
        return "ThumbnailGpuPreviewReadback";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPollReadback:
        return "ThumbnailGpuPreviewPollReadback";
    case ArtifactLoadTelemetryStage::ThumbnailTextureDecode:
        return "ThumbnailTextureDecode";
    case ArtifactLoadTelemetryStage::ThumbnailTextureUploadEnqueue:
        return "ThumbnailTextureUploadEnqueue";
    case ArtifactLoadTelemetryStage::ThumbnailTextureUpload:
        return "ThumbnailTextureUpload";
    case ArtifactLoadTelemetryStage::ThumbnailTextureUploadPreparePixels:
        return "ThumbnailTextureUploadPreparePixels";
    case ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreate:
        return "ThumbnailTextureUploadCreate";
    case ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreateView:
        return "ThumbnailTextureUploadCreateView";
    case ArtifactLoadTelemetryStage::ThumbnailTextureUploadSubmit:
        return "ThumbnailTextureUploadSubmit";
    case ArtifactLoadTelemetryStage::ThumbnailTextureUploadPublish:
        return "ThumbnailTextureUploadPublish";
    case ArtifactLoadTelemetryStage::ThumbnailTextureUploadResolveUiId:
        return "ThumbnailTextureUploadResolveUiId";
    case ArtifactLoadTelemetryStage::ThumbnailUiDraw:
        return "ThumbnailUiDraw";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGridVisibleRows:
        return "ThumbnailUiDrawGridVisibleRows";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemInteractions:
        return "ThumbnailUiDrawGridItemInteractions";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemThumbnail:
        return "ThumbnailUiDrawGridItemThumbnail";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemLabel:
        return "ThumbnailUiDrawGridItemLabel";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSet:
        return "ThumbnailUiDrawVisibleSet";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHash:
        return "ThumbnailUiDrawVisibleSetHash";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetApply:
        return "ThumbnailUiDrawVisibleSetApply";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHotCacheFlush:
        return "ThumbnailUiDrawVisibleSetHotCacheFlush";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScope:
        return "ThumbnailUiDrawGenerationScope";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeSelectItems:
        return "ThumbnailUiDrawGenerationScopeSelectItems";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildKey:
        return "ThumbnailUiDrawGenerationScopeBuildKey";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeItemKey:
        return "ThumbnailUiDrawGenerationScopeItemKey";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeResultLookup:
        return "ThumbnailUiDrawGenerationScopeResultLookup";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequest:
        return "ThumbnailUiDrawGenerationScopeBuildRequest";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestValidate:
        return "ThumbnailUiDrawGenerationScopeBuildRequestValidate";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestMetaId:
        return "ThumbnailUiDrawGenerationScopeBuildRequestMetaId";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestManifestLookup:
        return "ThumbnailUiDrawGenerationScopeBuildRequestManifestLookup";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestItemIdentity:
        return "ThumbnailUiDrawGenerationScopeBuildRequestItemIdentity";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshness:
        return "ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshness";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessResolve:
        return "ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessResolve";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessFileStamp:
        return "ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessFileStamp";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessMetaStamp:
        return "ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessMetaStamp";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestArtifactFreshness:
        return "ThumbnailUiDrawGenerationScopeBuildRequestArtifactFreshness";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestDependencyStamp:
        return "ThumbnailUiDrawGenerationScopeBuildRequestDependencyStamp";
    case ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeRequestPreview:
        return "ThumbnailUiDrawGenerationScopeRequestPreview";
    case ArtifactLoadTelemetryStage::ThumbnailServiceRequestStableLookup:
        return "ThumbnailServiceRequestStableLookup";
    case ArtifactLoadTelemetryStage::ThumbnailServiceRequestCacheEvaluate:
        return "ThumbnailServiceRequestCacheEvaluate";
    case ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue:
        return "ThumbnailServiceRequestQueue";
    case ArtifactLoadTelemetryStage::ThumbnailServiceGpuPreviewQueueDecision:
        return "ThumbnailServiceGpuPreviewQueueDecision";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePump:
        return "ThumbnailTexturePump";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpConsumeCompleted:
        return "ThumbnailTexturePumpConsumeCompleted";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadPoll:
        return "ThumbnailTexturePumpPendingUploadPoll";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadConsumeResult:
        return "ThumbnailTexturePumpPendingUploadConsumeResult";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadResolveUiId:
        return "ThumbnailTexturePumpPendingUploadResolveUiId";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadWrapTexture:
        return "ThumbnailTexturePumpPendingUploadWrapTexture";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadCachePublish:
        return "ThumbnailTexturePumpPendingUploadCachePublish";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodePoll:
        return "ThumbnailTexturePumpReadyDecodePoll";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodeLoad:
        return "ThumbnailTexturePumpReadyDecodeLoad";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpStartDecodes:
        return "ThumbnailTexturePumpStartDecodes";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpBuildResidentSet:
        return "ThumbnailTexturePumpBuildResidentSet";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpSelectDecodeCandidates:
        return "ThumbnailTexturePumpSelectDecodeCandidates";
    case ArtifactLoadTelemetryStage::ThumbnailTexturePumpScheduleDecodeJobs:
        return "ThumbnailTexturePumpScheduleDecodeJobs";
    case ArtifactLoadTelemetryStage::ThumbnailTextureUploadDeferred:
        return "ThumbnailTextureUploadDeferred";
    case ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPump:
        return "ThumbnailUiPostDrawPump";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareMaterialResources:
        return "ThumbnailGpuPreviewPrepareMaterialResources";
    case ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects:
        return "ThumbnailGpuPreviewPrepareSceneObjects";
    case ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpConsumeCompleted:
        return "ThumbnailUiPostDrawPumpConsumeCompleted";
    case ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpCreatePreviewRenderer:
        return "ThumbnailUiPostDrawPumpCreatePreviewRenderer";
    case ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartLightGpu:
        return "ThumbnailUiPostDrawPumpStartLightGpu";
    case ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartHeavyGpu:
        return "ThumbnailUiPostDrawPumpStartHeavyGpu";
    case ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartBackground:
        return "ThumbnailUiPostDrawPumpStartBackground";
    case ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntry:
        return "ThumbnailCacheEvaluateResolveEntry";
    case ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryBuild:
        return "ThumbnailCacheEvaluateResolveEntryBuild";
    case ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentKey:
        return "ThumbnailCacheEvaluateResolveEntryContainmentKey";
    case ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentStamp:
        return "ThumbnailCacheEvaluateResolveEntryContainmentStamp";
    case ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentValidate:
        return "ThumbnailCacheEvaluateResolveEntryContainmentValidate";
    case ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateMetadataStat:
        return "ThumbnailCacheEvaluateMetadataStat";
    case ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateMetadataLoad:
        return "ThumbnailCacheEvaluateMetadataLoad";
    case ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateFreshness:
        return "ThumbnailCacheEvaluateFreshness";
    case ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateImageStat:
        return "ThumbnailCacheEvaluateImageStat";
    case ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateImageValidate:
        return "ThumbnailCacheEvaluateImageValidate";
    }
    return "Unknown";
}
}
