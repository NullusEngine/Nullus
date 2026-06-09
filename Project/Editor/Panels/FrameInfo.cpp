#include "Panels/FrameInfo.h"

#include <algorithm>

using namespace NLS;

namespace
{
void AddLargeSceneTelemetry(
    NLS::Render::Data::LargeSceneTelemetry& aggregate,
    const NLS::Render::Data::LargeSceneTelemetry& value)
{
    aggregate.registeredPrimitiveCount += value.registeredPrimitiveCount;
    aggregate.staticPrimitiveCount += value.staticPrimitiveCount;
    aggregate.dynamicPrimitiveCount += value.dynamicPrimitiveCount;
    aggregate.unclassifiedPrimitiveCount += value.unclassifiedPrimitiveCount;
    aggregate.spatialCandidateCount += value.spatialCandidateCount;
    aggregate.fullScanCandidateCount += value.fullScanCandidateCount;
    aggregate.visiblePrimitiveCount += value.visiblePrimitiveCount;
    aggregate.visibleMeshCount += value.visibleMeshCount;
    for (size_t i = 0u; i < aggregate.culledByReason.size(); ++i)
        aggregate.culledByReason[i] += value.culledByReason[i];
    for (size_t i = 0u; i < aggregate.lodSelectionCount.size(); ++i)
        aggregate.lodSelectionCount[i] += value.lodSelectionCount[i];
    aggregate.activeHLODClusterCount += value.activeHLODClusterCount;
    aggregate.occlusionTestCount += value.occlusionTestCount;
    aggregate.occlusionCulledCount += value.occlusionCulledCount;
    aggregate.streamingRequestCount += value.streamingRequestCount;
    aggregate.streamingCommitCount += value.streamingCommitCount;
    aggregate.streamingEvictCount += value.streamingEvictCount;
    aggregate.streamingDependencyCount += value.streamingDependencyCount;
    aggregate.residencyTicketCount += value.residencyTicketCount;
    aggregate.residentCpuBytes += value.residentCpuBytes;
    aggregate.residentGpuBytes += value.residentGpuBytes;
    aggregate.requestedCpuBytes += value.requestedCpuBytes;
    aggregate.requestedGpuBytes += value.requestedGpuBytes;
    aggregate.primitiveRecordsTouched += value.primitiveRecordsTouched;
    aggregate.allocatedPrimitiveSlotCount += value.allocatedPrimitiveSlotCount;
    aggregate.tombstonedPrimitiveSlotCount += value.tombstonedPrimitiveSlotCount;
    aggregate.syncSweepTouchedSlotCount += value.syncSweepTouchedSlotCount;
    aggregate.syncTouchedPrimitiveCount += value.syncTouchedPrimitiveCount;
    aggregate.syncFullSweepCount += value.syncFullSweepCount;
    aggregate.boundsDirtyPrimitiveCount += value.boundsDirtyPrimitiveCount;
    aggregate.primitiveSlotReuseCount += value.primitiveSlotReuseCount;
    aggregate.visibilityTestedPrimitiveCount += value.visibilityTestedPrimitiveCount;
    aggregate.visibilityBitsetWordCount += value.visibilityBitsetWordCount;
    aggregate.finalizationTouchedPrimitiveCount += value.finalizationTouchedPrimitiveCount;
    aggregate.finalizationTouchedCommandCount += value.finalizationTouchedCommandCount;
    aggregate.commandOffsetRebuildCount += value.commandOffsetRebuildCount;
    aggregate.rawVisibleDrawCount += value.rawVisibleDrawCount;
    aggregate.submittedDrawCount += value.submittedDrawCount;
    aggregate.dynamicInstanceGroupCount += value.dynamicInstanceGroupCount;
    aggregate.dynamicCandidateCount += value.dynamicCandidateCount;
    aggregate.dynamicRecordsTouched += value.dynamicRecordsTouched;
    aggregate.staticIndexRefitCount += value.staticIndexRefitCount;
    aggregate.staticIndexRebuildCount += value.staticIndexRebuildCount;
    aggregate.staticIndexLastGoodQueryCount += value.staticIndexLastGoodQueryCount;
    aggregate.staticIndexDirtyOverlayCount += value.staticIndexDirtyOverlayCount;
    aggregate.spatialRebuildFallbackCount += value.spatialRebuildFallbackCount;
    aggregate.dynamicIndexUpdateCount += value.dynamicIndexUpdateCount;
    aggregate.syncTimeNs += value.syncTimeNs;
    aggregate.serialVisibilityTimeNs += value.serialVisibilityTimeNs;
    aggregate.parallelVisibilityTimeNs += value.parallelVisibilityTimeNs;
    aggregate.queueFinalizationTimeNs += value.queueFinalizationTimeNs;
    aggregate.hzbBuildTimeNs += value.hzbBuildTimeNs;
    aggregate.hzbHistoryPruneTouchedHandleCount += value.hzbHistoryPruneTouchedHandleCount;
    aggregate.hzbHistoryPruneRemovedHandleCount += value.hzbHistoryPruneRemovedHandleCount;
    aggregate.hzbHistoryPruneRemovedKeyCount += value.hzbHistoryPruneRemovedKeyCount;
    aggregate.hzbHistoryPruneTimeNs += value.hzbHistoryPruneTimeNs;
    aggregate.streamingCommitTimeNs += value.streamingCommitTimeNs;
}

void AddFrameInfo(
    NLS::Render::Data::FrameInfo& aggregate,
    const NLS::Render::Data::FrameInfo& value)
{
    aggregate.batchCount += value.batchCount;
    aggregate.instanceCount += value.instanceCount;
    aggregate.polyCount += value.polyCount;
    aggregate.vertexCount += value.vertexCount;
    aggregate.inFlightFrameCount += value.inFlightFrameCount;
    aggregate.blockedFrameCount += value.blockedFrameCount;
    aggregate.reservedSlotWaitCount += value.reservedSlotWaitCount;
    aggregate.reservedSlotWaitTimeoutCount += value.reservedSlotWaitTimeoutCount;
    aggregate.reservedSlotWaitTotalNs += value.reservedSlotWaitTotalNs;
    if (value.publishState != NLS::Render::Data::FramePublishState::Direct)
        aggregate.publishState = value.publishState;
    if (value.stageSummary != NLS::Render::Data::ThreadedFrameStageSummary::Direct)
        aggregate.stageSummary = value.stageSummary;
    if (value.retirementState != NLS::Render::Data::FrameRetirementState::Direct)
        aggregate.retirementState = value.retirementState;
    aggregate.descriptorMainlineActive = aggregate.descriptorMainlineActive || value.descriptorMainlineActive;
    aggregate.pipelineMainlineActive = aggregate.pipelineMainlineActive || value.pipelineMainlineActive;
    aggregate.transientLifetimeMainlineActive = aggregate.transientLifetimeMainlineActive || value.transientLifetimeMainlineActive;
    aggregate.retirementMainlineActive = aggregate.retirementMainlineActive || value.retirementMainlineActive;
    aggregate.descriptorBypassCount += value.descriptorBypassCount;
    aggregate.pipelineBypassCount += value.pipelineBypassCount;
    aggregate.transientLifetimeBypassCount += value.transientLifetimeBypassCount;
    aggregate.retirementBypassCount += value.retirementBypassCount;
    aggregate.transientTextureRegistrationCount += value.transientTextureRegistrationCount;
    aggregate.transientBufferRegistrationCount += value.transientBufferRegistrationCount;
    aggregate.retiredTransientTextureCount += value.retiredTransientTextureCount;
    aggregate.retiredTransientBufferCount += value.retiredTransientBufferCount;
    aggregate.descriptorTransientPeak = std::max(aggregate.descriptorTransientPeak, value.descriptorTransientPeak);
    aggregate.descriptorAllocationFailures += value.descriptorAllocationFailures;
    aggregate.pipelineCacheGraphicsHits += value.pipelineCacheGraphicsHits;
    aggregate.pipelineCacheGraphicsMisses += value.pipelineCacheGraphicsMisses;
    aggregate.pipelineCacheGraphicsStores += value.pipelineCacheGraphicsStores;
    aggregate.pipelineCacheGraphicsEntries += value.pipelineCacheGraphicsEntries;
    aggregate.pipelineCacheComputeHits += value.pipelineCacheComputeHits;
    aggregate.pipelineCacheComputeMisses += value.pipelineCacheComputeMisses;
    aggregate.pipelineCacheComputeStores += value.pipelineCacheComputeStores;
    aggregate.pipelineCacheComputeEntries += value.pipelineCacheComputeEntries;
    aggregate.parseSceneCallCount += value.parseSceneCallCount;
    aggregate.parsedOpaqueDrawableCount += value.parsedOpaqueDrawableCount;
    aggregate.parsedTransparentDrawableCount += value.parsedTransparentDrawableCount;
    aggregate.parsedSkyboxDrawableCount += value.parsedSkyboxDrawableCount;
    aggregate.gBufferMaterialSyncCount += value.gBufferMaterialSyncCount;
    aggregate.gBufferMaterialResolveHitCount += value.gBufferMaterialResolveHitCount;
    aggregate.gBufferMaterialResolveMissCount += value.gBufferMaterialResolveMissCount;
    aggregate.preparedRecordedDrawStaticBaseCacheHitCount += value.preparedRecordedDrawStaticBaseCacheHitCount;
    aggregate.preparedRecordedDrawStaticBaseCacheMissCount += value.preparedRecordedDrawStaticBaseCacheMissCount;
    aggregate.renderBindingSetCreationCount += value.renderBindingSetCreationCount;
    aggregate.renderSnapshotBufferCreationCount += value.renderSnapshotBufferCreationCount;
    aggregate.rawVisibleObjectCount += value.rawVisibleObjectCount;
    aggregate.submittedSceneDrawCount += value.submittedSceneDrawCount;
    aggregate.dynamicInstanceGroupCount += value.dynamicInstanceGroupCount;
    aggregate.largestInstanceGroupSize = std::max(aggregate.largestInstanceGroupSize, value.largestInstanceGroupSize);
    aggregate.cachedCommandRebuildCount += value.cachedCommandRebuildCount;
    aggregate.objectDataOverflowDroppedObjectCount += value.objectDataOverflowDroppedObjectCount;
    aggregate.parallelCommandWorkUnitCount += value.parallelCommandWorkUnitCount;
    aggregate.parallelRecordingWorkerCount += value.parallelRecordingWorkerCount;
    if (aggregate.parallelFallbackReason.empty())
        aggregate.parallelFallbackReason = value.parallelFallbackReason;
    aggregate.deviceLostDetected = aggregate.deviceLostDetected || value.deviceLostDetected;
    if (aggregate.deviceLostReason.empty())
        aggregate.deviceLostReason = value.deviceLostReason;
    aggregate.unsafeGpuWorkQuarantined = aggregate.unsafeGpuWorkQuarantined || value.unsafeGpuWorkQuarantined;
    if (aggregate.unsafeGpuWorkQuarantineReason.empty())
        aggregate.unsafeGpuWorkQuarantineReason = value.unsafeGpuWorkQuarantineReason;
    AddLargeSceneTelemetry(aggregate.largeScene, value.largeScene);
}
}

void Editor::Panels::FrameInfo::SetTargetView(AView* p_targetView)
{
	m_targetView = p_targetView;
}

void Editor::Panels::FrameInfo::SetCandidateViews(std::vector<AView*> candidateViews)
{
	m_candidateViews = std::move(candidateViews);
}

Editor::Panels::AView* Editor::Panels::FrameInfo::GetTargetView() const
{
	return m_targetView;
}

void Editor::Panels::FrameInfo::OnBeforeDrawWidgets()
{
	if (!m_candidateViews.empty())
	{
		RefreshForViews(m_candidateViews);
		return;
	}

	RefreshForView(m_targetView);
}

void Editor::Panels::FrameInfo::RefreshForView(AView* p_targetView)
{
	static const Render::Data::FrameInfo kEmptyFrameInfo;
	if (p_targetView == nullptr || !p_targetView->IsOpened())
	{
		UpdateForFrameInfo("None", kEmptyFrameInfo);
		return;
	}

	const auto& frameInfo = p_targetView->GetLastRenderedFrameInfoSnapshot();
	UpdateForFrameInfo(
		p_targetView->name,
		frameInfo.has_value() ? frameInfo.value() : kEmptyFrameInfo);
}

void Editor::Panels::FrameInfo::RefreshForViews(const std::vector<AView*>& targetViews)
{
	static const Render::Data::FrameInfo kEmptyFrameInfo;
	std::vector<FrameInfoViewSnapshot> snapshots;
	snapshots.reserve(targetViews.size());

	for (auto* targetView : targetViews)
	{
		if (targetView == nullptr || !targetView->IsOpened() || !targetView->WasViewportImageDrawnThisFrame())
			continue;

		const auto& frameInfo = targetView->GetLastRenderedFrameInfoSnapshot();
		snapshots.push_back({
			targetView->name,
			frameInfo.has_value() ? frameInfo.value() : kEmptyFrameInfo
		});
	}

	UpdateForFrameInfoViews(snapshots);
}

void Editor::Panels::FrameInfo::UpdateForFrameInfoViews(const std::vector<FrameInfoViewSnapshot>& viewSnapshots)
{
	static const Render::Data::FrameInfo kEmptyFrameInfo;
	if (viewSnapshots.empty())
	{
		UpdateForFrameInfo("None", kEmptyFrameInfo);
		return;
	}

	if (viewSnapshots.size() == 1u)
	{
		UpdateForFrameInfo(viewSnapshots.front().viewName, viewSnapshots.front().frameInfo);
		return;
	}

	Render::Data::FrameInfo aggregate;
	std::string viewName;
	for (const auto& snapshot : viewSnapshots)
	{
		if (!viewName.empty())
			viewName += " + ";
		viewName += snapshot.viewName;
		AddFrameInfo(aggregate, snapshot.frameInfo);
	}

	UpdateForFrameInfo(viewName, aggregate);
}
