#include "Rendering/Core/RendererStats.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/DrawCallOptimizationStats.h"

#include <algorithm>

namespace NLS::Render::Core
{
namespace
{
    template <size_t Count>
    void AccumulateArray(
        std::array<uint64_t, Count>& target,
        const std::array<uint64_t, Count>& source)
    {
        for (size_t index = 0u; index < Count; ++index)
            target[index] += source[index];
    }

    template<typename TTelemetry>
    uint64_t ResolveBlockedFrameCount(const TTelemetry& telemetry)
    {
        if constexpr (requires { telemetry.blockedPublishCount; })
            return telemetry.blockedPublishCount;
        else
            return telemetry.blockedFrameCount;
    }

    template<typename TTelemetry>
    void ApplyThreadedFrameTelemetryFields(const TTelemetry& telemetry, Data::FrameInfo& frameInfo)
    {
        frameInfo.inFlightFrameCount = telemetry.inFlightFrameCount;
        frameInfo.blockedFrameCount = ResolveBlockedFrameCount(telemetry);
        frameInfo.reservedSlotWaitCount = telemetry.reservedSlotWaitCount;
        frameInfo.reservedSlotWaitTimeoutCount = telemetry.reservedSlotWaitTimeoutCount;
        frameInfo.reservedSlotWaitTotalNs = telemetry.reservedSlotWaitTotalNs;
        frameInfo.publishState = telemetry.publishState;
        frameInfo.stageSummary = telemetry.stageSummary;
        frameInfo.retirementState = telemetry.retirementState;
        frameInfo.descriptorMainlineActive = telemetry.descriptorMainlineActive;
        frameInfo.pipelineMainlineActive = telemetry.pipelineMainlineActive;
        frameInfo.transientLifetimeMainlineActive = telemetry.transientLifetimeMainlineActive;
        frameInfo.retirementMainlineActive = telemetry.retirementMainlineActive;
        frameInfo.descriptorBypassCount = telemetry.descriptorBypassCount;
        frameInfo.pipelineBypassCount = telemetry.pipelineBypassCount;
        frameInfo.transientLifetimeBypassCount = telemetry.transientLifetimeBypassCount;
        frameInfo.retirementBypassCount = telemetry.retirementBypassCount;
        frameInfo.transientTextureRegistrationCount = telemetry.transientTextureRegistrationCount;
        frameInfo.transientBufferRegistrationCount = telemetry.transientBufferRegistrationCount;
        frameInfo.retiredTransientTextureCount = telemetry.retiredTransientTextureCount;
        frameInfo.retiredTransientBufferCount = telemetry.retiredTransientBufferCount;
        frameInfo.descriptorTransientPeak = telemetry.descriptorTransientPeak;
        frameInfo.descriptorAllocationFailures = telemetry.descriptorAllocationFailures;
        frameInfo.pipelineCacheGraphicsHits = telemetry.pipelineCacheGraphicsHits;
        frameInfo.pipelineCacheGraphicsMisses = telemetry.pipelineCacheGraphicsMisses;
        frameInfo.pipelineCacheGraphicsStores = telemetry.pipelineCacheGraphicsStores;
        frameInfo.pipelineCacheGraphicsEntries = telemetry.pipelineCacheGraphicsEntries;
        frameInfo.pipelineCacheComputeHits = telemetry.pipelineCacheComputeHits;
        frameInfo.pipelineCacheComputeMisses = telemetry.pipelineCacheComputeMisses;
        frameInfo.pipelineCacheComputeStores = telemetry.pipelineCacheComputeStores;
        frameInfo.pipelineCacheComputeEntries = telemetry.pipelineCacheComputeEntries;
        frameInfo.parallelCommandWorkUnitCount = telemetry.parallelCommandWorkUnitCount;
        frameInfo.parallelRecordingWorkerCount = telemetry.parallelRecordingWorkerCount;
        frameInfo.parallelFallbackReason = telemetry.parallelFallbackReason;
        frameInfo.deviceLostDetected = telemetry.deviceLostDetected;
        frameInfo.deviceLostReason = telemetry.deviceLostReason;
        frameInfo.unsafeGpuWorkQuarantined = telemetry.unsafeGpuWorkQuarantined;
        frameInfo.unsafeGpuWorkQuarantineReason = telemetry.unsafeGpuWorkQuarantineReason;
    }
}

void RendererStats::BeginFrame()
{
    m_frameInfo.batchCount = 0u;
    m_frameInfo.instanceCount = 0u;
    m_frameInfo.polyCount = 0u;
    m_frameInfo.vertexCount = 0u;
    m_frameInfo.inFlightFrameCount = 0u;
    m_frameInfo.blockedFrameCount = 0u;
    m_frameInfo.reservedSlotWaitCount = 0u;
    m_frameInfo.reservedSlotWaitTimeoutCount = 0u;
    m_frameInfo.reservedSlotWaitTotalNs = 0u;
    m_frameInfo.publishState = Data::FramePublishState::Direct;
    m_frameInfo.stageSummary = Data::ThreadedFrameStageSummary::Direct;
    m_frameInfo.retirementState = Data::FrameRetirementState::Direct;
    m_frameInfo.descriptorMainlineActive = false;
    m_frameInfo.pipelineMainlineActive = false;
    m_frameInfo.transientLifetimeMainlineActive = false;
    m_frameInfo.retirementMainlineActive = false;
    m_frameInfo.descriptorBypassCount = 0u;
    m_frameInfo.pipelineBypassCount = 0u;
    m_frameInfo.transientLifetimeBypassCount = 0u;
    m_frameInfo.retirementBypassCount = 0u;
    m_frameInfo.transientTextureRegistrationCount = 0u;
    m_frameInfo.transientBufferRegistrationCount = 0u;
    m_frameInfo.retiredTransientTextureCount = 0u;
    m_frameInfo.retiredTransientBufferCount = 0u;
    m_frameInfo.descriptorTransientPeak = 0u;
    m_frameInfo.descriptorAllocationFailures = 0u;
    m_frameInfo.pipelineCacheGraphicsHits = 0u;
    m_frameInfo.pipelineCacheGraphicsMisses = 0u;
    m_frameInfo.pipelineCacheGraphicsStores = 0u;
    m_frameInfo.pipelineCacheGraphicsEntries = 0u;
    m_frameInfo.pipelineCacheComputeHits = 0u;
    m_frameInfo.pipelineCacheComputeMisses = 0u;
    m_frameInfo.pipelineCacheComputeStores = 0u;
    m_frameInfo.pipelineCacheComputeEntries = 0u;
    m_frameInfo.parseSceneCallCount = 0u;
    m_frameInfo.parsedOpaqueDrawableCount = 0u;
    m_frameInfo.parsedTransparentDrawableCount = 0u;
    m_frameInfo.parsedSkyboxDrawableCount = 0u;
    m_frameInfo.gBufferMaterialSyncCount = 0u;
    m_frameInfo.gBufferMaterialResolveHitCount = 0u;
    m_frameInfo.gBufferMaterialResolveMissCount = 0u;
    m_frameInfo.preparedRecordedDrawStaticBaseCacheHitCount = 0u;
    m_frameInfo.preparedRecordedDrawStaticBaseCacheMissCount = 0u;
    m_frameInfo.renderBindingSetCreationCount = 0u;
    m_frameInfo.renderSnapshotBufferCreationCount = 0u;
    m_frameInfo.rawVisibleObjectCount = 0u;
    m_frameInfo.submittedSceneDrawCount = 0u;
    m_frameInfo.dynamicInstanceGroupCount = 0u;
    m_frameInfo.largestInstanceGroupSize = 0u;
    m_frameInfo.cachedCommandRebuildCount = 0u;
    m_frameInfo.objectDataOverflowDroppedObjectCount = 0u;
    m_frameInfo.parallelCommandWorkUnitCount = 0u;
    m_frameInfo.parallelRecordingWorkerCount = 0u;
    m_frameInfo.parallelFallbackReason.clear();
    m_frameInfo.deviceLostDetected = false;
    m_frameInfo.deviceLostReason.clear();
    m_frameInfo.unsafeGpuWorkQuarantined = false;
    m_frameInfo.unsafeGpuWorkQuarantineReason.clear();
    m_frameInfo.largeScene = {};
    m_isFrameInfoValid = false;
}

void RendererStats::EndFrame()
{
    m_isFrameInfoValid = true;
}

void RendererStats::RecordSubmittedDraw(const Entities::Drawable& drawable, const uint32_t instanceCount)
{
    if (drawable.mesh == nullptr || drawable.material == nullptr || instanceCount == 0u)
        return;

    constexpr uint32_t kVertexCountPerPolygon = 3u;

    ++m_frameInfo.batchCount;
    m_frameInfo.instanceCount += instanceCount;
    m_frameInfo.polyCount += (drawable.mesh->GetIndexCount() / kVertexCountPerPolygon) * instanceCount;
    const auto drawableVertexCount = drawable.vertexCount != 0u
        ? drawable.vertexCount
        : drawable.mesh->GetVertexCount();
    m_frameInfo.vertexCount += drawableVertexCount * instanceCount;
}

void RendererStats::RecordSceneParse(
    const uint64_t opaqueCount,
    const uint64_t transparentCount,
    const uint64_t skyboxCount)
{
    ++m_frameInfo.parseSceneCallCount;
    m_frameInfo.parsedOpaqueDrawableCount = opaqueCount;
    m_frameInfo.parsedTransparentDrawableCount = transparentCount;
    m_frameInfo.parsedSkyboxDrawableCount = skyboxCount;
}

void RendererStats::RecordGBufferMaterialSync()
{
    ++m_frameInfo.gBufferMaterialSyncCount;
}

void RendererStats::RecordGBufferMaterialResolve(const bool hit)
{
    if (hit)
        ++m_frameInfo.gBufferMaterialResolveHitCount;
    else
        ++m_frameInfo.gBufferMaterialResolveMissCount;
}

void RendererStats::RecordPreparedRecordedDrawStaticBaseCache(const bool hit)
{
    if (hit)
        ++m_frameInfo.preparedRecordedDrawStaticBaseCacheHitCount;
    else
        ++m_frameInfo.preparedRecordedDrawStaticBaseCacheMissCount;
}

void RendererStats::RecordRenderBindingSetCreation(const uint64_t count)
{
    m_frameInfo.renderBindingSetCreationCount += count;
}

void RendererStats::RecordRenderSnapshotBufferCreation(const uint64_t count)
{
    m_frameInfo.renderSnapshotBufferCreationCount += count;
}

void RendererStats::RecordDrawCallOptimizationStats(
    const NLS::Render::Data::DrawCallOptimizationStats& stats)
{
    m_frameInfo.rawVisibleObjectCount = stats.rawVisibleObjectCount;
    m_frameInfo.submittedSceneDrawCount = stats.submittedSceneDrawCount;
    m_frameInfo.dynamicInstanceGroupCount = stats.dynamicInstanceGroupCount;
    m_frameInfo.largestInstanceGroupSize = stats.largestInstanceGroupSize;
    m_frameInfo.cachedCommandRebuildCount = stats.cachedCommandRebuildCount;
    m_frameInfo.objectDataOverflowDroppedObjectCount = stats.objectDataOverflowDroppedObjectCount;
}

void RendererStats::RecordLargeSceneTelemetry(
    const NLS::Render::Data::LargeSceneTelemetry& telemetry)
{
    auto& target = m_frameInfo.largeScene;
    target.registeredPrimitiveCount += telemetry.registeredPrimitiveCount;
    target.staticPrimitiveCount += telemetry.staticPrimitiveCount;
    target.dynamicPrimitiveCount += telemetry.dynamicPrimitiveCount;
    target.unclassifiedPrimitiveCount += telemetry.unclassifiedPrimitiveCount;
    target.spatialCandidateCount += telemetry.spatialCandidateCount;
    target.fullScanCandidateCount += telemetry.fullScanCandidateCount;
    target.visiblePrimitiveCount += telemetry.visiblePrimitiveCount;
    target.visibleMeshCount += telemetry.visibleMeshCount;
    AccumulateArray(target.culledByReason, telemetry.culledByReason);
    AccumulateArray(target.lodSelectionCount, telemetry.lodSelectionCount);
    target.activeHLODClusterCount += telemetry.activeHLODClusterCount;
    target.occlusionTestCount += telemetry.occlusionTestCount;
    target.occlusionCulledCount += telemetry.occlusionCulledCount;
    target.streamingRequestCount += telemetry.streamingRequestCount;
    target.streamingCommitCount += telemetry.streamingCommitCount;
    target.streamingEvictCount += telemetry.streamingEvictCount;
    target.streamingDependencyCount += telemetry.streamingDependencyCount;
    target.residencyTicketCount = telemetry.residencyTicketCount;
    target.residentCpuBytes = telemetry.residentCpuBytes;
    target.residentGpuBytes = telemetry.residentGpuBytes;
    target.requestedCpuBytes = telemetry.requestedCpuBytes;
    target.requestedGpuBytes = telemetry.requestedGpuBytes;
    target.primitiveRecordsTouched += telemetry.primitiveRecordsTouched;
    target.allocatedPrimitiveSlotCount += telemetry.allocatedPrimitiveSlotCount;
    target.tombstonedPrimitiveSlotCount += telemetry.tombstonedPrimitiveSlotCount;
    target.syncSweepTouchedSlotCount += telemetry.syncSweepTouchedSlotCount;
    target.syncTouchedPrimitiveCount += telemetry.syncTouchedPrimitiveCount;
    target.syncFullSweepCount += telemetry.syncFullSweepCount;
    target.boundsDirtyPrimitiveCount += telemetry.boundsDirtyPrimitiveCount;
    target.primitiveSlotReuseCount += telemetry.primitiveSlotReuseCount;
    target.visibilityTestedPrimitiveCount += telemetry.visibilityTestedPrimitiveCount;
    target.visibilityBitsetWordCount += telemetry.visibilityBitsetWordCount;
    target.finalizationTouchedPrimitiveCount += telemetry.finalizationTouchedPrimitiveCount;
    target.finalizationTouchedCommandCount += telemetry.finalizationTouchedCommandCount;
    target.commandOffsetRebuildCount += telemetry.commandOffsetRebuildCount;
    target.rawVisibleDrawCount += telemetry.rawVisibleDrawCount;
    target.submittedDrawCount += telemetry.submittedDrawCount;
    target.dynamicInstanceGroupCount += telemetry.dynamicInstanceGroupCount;
    target.dynamicCandidateCount += telemetry.dynamicCandidateCount;
    target.dynamicRecordsTouched += telemetry.dynamicRecordsTouched;
    target.staticIndexRefitCount += telemetry.staticIndexRefitCount;
    target.staticIndexRebuildCount += telemetry.staticIndexRebuildCount;
    target.staticIndexLastGoodQueryCount += telemetry.staticIndexLastGoodQueryCount;
    target.staticIndexDirtyOverlayCount += telemetry.staticIndexDirtyOverlayCount;
    target.spatialRebuildFallbackCount += telemetry.spatialRebuildFallbackCount;
    target.dynamicIndexUpdateCount += telemetry.dynamicIndexUpdateCount;
    target.syncTimeNs += telemetry.syncTimeNs;
    target.serialVisibilityTimeNs += telemetry.serialVisibilityTimeNs;
    target.parallelVisibilityTimeNs += telemetry.parallelVisibilityTimeNs;
    target.queueFinalizationTimeNs += telemetry.queueFinalizationTimeNs;
    target.hzbBuildTimeNs += telemetry.hzbBuildTimeNs;
    target.hzbHistoryPruneTouchedHandleCount += telemetry.hzbHistoryPruneTouchedHandleCount;
    target.hzbHistoryPruneRemovedHandleCount += telemetry.hzbHistoryPruneRemovedHandleCount;
    target.hzbHistoryPruneRemovedKeyCount += telemetry.hzbHistoryPruneRemovedKeyCount;
    target.hzbHistoryPruneTimeNs += telemetry.hzbHistoryPruneTimeNs;
    target.streamingCommitTimeNs += telemetry.streamingCommitTimeNs;
}

void RendererStats::SetThreadedFrameTelemetry(const NLS::Render::Context::ThreadedFrameTelemetry& telemetry)
{
    ApplyThreadedFrameTelemetry(telemetry, m_frameInfo);
    m_lastThreadedFrameInfoTelemetry = m_frameInfo;
}

bool RendererStats::ReuseLastThreadedFrameTelemetry()
{
    if (!m_lastThreadedFrameInfoTelemetry.has_value())
        return false;

    ApplyThreadedFrameTelemetryFields(m_lastThreadedFrameInfoTelemetry.value(), m_frameInfo);
    return true;
}

void RendererStats::ApplyThreadedFrameTelemetry(
    const NLS::Render::Context::ThreadedFrameTelemetry& telemetry,
    Data::FrameInfo& frameInfo)
{
    ApplyThreadedFrameTelemetryFields(telemetry, frameInfo);
}

const Data::FrameInfo& RendererStats::GetFrameInfo() const
{
    NLS_ASSERT(m_isFrameInfoValid, "Invalid FrameInfo data! Make sure to retrieve frame info after the frame got fully rendered");
    return m_frameInfo;
}

bool RendererStats::IsFrameInfoValid() const
{
    return m_isFrameInfoValid;
}
}
