#include "Panels/FrameInfo.h"

#include "Rendering/SceneVisibilityPipeline.h"
#include "Utils/String.h"

using namespace NLS;
using namespace NLS::UI;

namespace
{
	const char* ToFramePublishStateText(const Render::Data::FramePublishState publishState)
	{
		switch (publishState)
		{
		case Render::Data::FramePublishState::Direct:
			return "Direct";
		case Render::Data::FramePublishState::Open:
			return "Open";
		case Render::Data::FramePublishState::BackPressured:
			return "BackPressured";
		default:
			return "Unknown";
		}
	}

	const char* ToThreadedFrameStageSummaryText(const Render::Data::ThreadedFrameStageSummary stageSummary)
	{
		switch (stageSummary)
		{
		case Render::Data::ThreadedFrameStageSummary::Direct:
			return "Direct";
		case Render::Data::ThreadedFrameStageSummary::Logic:
			return "Logic";
		case Render::Data::ThreadedFrameStageSummary::RenderScene:
			return "RenderScene";
		case Render::Data::ThreadedFrameStageSummary::Rhi:
			return "RHI";
		case Render::Data::ThreadedFrameStageSummary::Retired:
			return "Retired";
		default:
			return "Unknown";
		}
	}

	const char* ToFrameRetirementStateText(const Render::Data::FrameRetirementState retirementState)
	{
		switch (retirementState)
		{
		case Render::Data::FrameRetirementState::Direct:
			return "Direct";
		case Render::Data::FrameRetirementState::Pending:
			return "Pending";
		case Render::Data::FrameRetirementState::Ready:
			return "Ready";
		case Render::Data::FrameRetirementState::Consumed:
			return "Consumed";
		default:
			return "Unknown";
		}
	}

	std::string BuildLargeSceneCullReasonText(
		const std::array<uint64_t, Render::Data::kLargeSceneCullReasonCount>& counts)
	{
		std::string content = "Large Scene Cull ";
		std::string values;
		bool firstBucket = true;
		for (const auto& bucket : Engine::Rendering::SceneVisibilityPipeline::GetCullReasonDisplayBuckets())
		{
			if (!firstBucket)
			{
				content += "/";
				values += "/";
			}
			firstBucket = false;

			content += bucket.label;
			uint64_t bucketCount = 0u;
			for (size_t reasonIndex = 0u; reasonIndex < bucket.reasonCount; ++reasonIndex)
			{
				const auto denseReason = static_cast<size_t>(bucket.reasons[reasonIndex]);
				if (denseReason < counts.size())
					bucketCount += counts[denseReason];
			}
			values += Utils::String::ToString(bucketCount);
		}
		content += ": ";
		content += values;
		return content;
	}
}

Editor::Panels::FrameInfo::FrameInfo
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) : PanelWindow(p_title, p_opened, p_windowSettings),

	m_viewNameText(CreateWidget<Widgets::Text>()),

	m_separator(CreateWidget<Widgets::Separator>()),

	m_batchCountText(CreateWidget<Widgets::Text>("")),
	m_instanceCountText(CreateWidget<Widgets::Text>("")),
	m_polyCountText(CreateWidget<Widgets::Text>("")),
	m_vertexCountText(CreateWidget<Widgets::Text>("")),
	m_parseSceneText(CreateWidget<Widgets::Text>("")),
	m_drawableCountText(CreateWidget<Widgets::Text>("")),
	m_drawCallOptimizationText(CreateWidget<Widgets::Text>("")),
	m_parallelWorkText(CreateWidget<Widgets::Text>("")),
	m_largeScenePrimitiveText(CreateWidget<Widgets::Text>("")),
	m_largeSceneVisibilityText(CreateWidget<Widgets::Text>("")),
	m_largeSceneCullReasonText(CreateWidget<Widgets::Text>("")),
	m_largeSceneSyncText(CreateWidget<Widgets::Text>("")),
	m_largeSceneFinalizationText(CreateWidget<Widgets::Text>("")),
	m_largeSceneResidencyText(CreateWidget<Widgets::Text>("")),
	m_largeSceneStreamingText(CreateWidget<Widgets::Text>("")),
	m_largeSceneHZBHistoryPruneText(CreateWidget<Widgets::Text>("")),
	m_largeSceneTimingText(CreateWidget<Widgets::Text>("")),
	m_gBufferMaterialSyncText(CreateWidget<Widgets::Text>("")),
	m_gBufferMaterialResolveText(CreateWidget<Widgets::Text>("")),
	m_preparedDrawStaticBaseCacheText(CreateWidget<Widgets::Text>("")),
	m_bindingSetCreationText(CreateWidget<Widgets::Text>("")),
	m_snapshotBufferCreationText(CreateWidget<Widgets::Text>("")),
	m_framesInFlightText(CreateWidget<Widgets::Text>("")),
	m_blockedFramesText(CreateWidget<Widgets::Text>("")),
	m_publishStateText(CreateWidget<Widgets::Text>("")),
	m_frameStageText(CreateWidget<Widgets::Text>("")),
	m_retirementStateText(CreateWidget<Widgets::Text>("")),
	m_rhiSafetyText(CreateWidget<Widgets::Text>(""))
{
	m_polyCountText.lineBreak = false;
}

void Editor::Panels::FrameInfo::UpdateForFrameInfo(
	const std::string& viewName,
	const Render::Data::FrameInfo& frameInfo)
{
	using namespace Utils;

	m_viewNameText.content = "Target View: " + viewName;

	m_batchCountText.content = "Batches: " + String::ToString(frameInfo.batchCount);
	m_instanceCountText.content = "Instances: " + String::ToString(frameInfo.instanceCount);
	m_polyCountText.content = "Polygons: " + String::ToString(frameInfo.polyCount);
	m_vertexCountText.content = "Vertices: " + String::ToString(frameInfo.vertexCount);
	m_parseSceneText.content = "ParseScene Calls: " + String::ToString(frameInfo.parseSceneCallCount);
	m_drawableCountText.content =
		"Drawables O/T/S: " +
		String::ToString(frameInfo.parsedOpaqueDrawableCount) + "/" +
		String::ToString(frameInfo.parsedTransparentDrawableCount) + "/" +
		String::ToString(frameInfo.parsedSkyboxDrawableCount);
	m_drawCallOptimizationText.content =
		"Draw Opt Raw/Submitted/Groups/Largest/Rebuilds/Dropped: " +
		String::ToString(frameInfo.rawVisibleObjectCount) + "/" +
		String::ToString(frameInfo.submittedSceneDrawCount) + "/" +
		String::ToString(frameInfo.dynamicInstanceGroupCount) + "/" +
		String::ToString(frameInfo.largestInstanceGroupSize) + "/" +
		String::ToString(frameInfo.cachedCommandRebuildCount) + "/" +
		String::ToString(frameInfo.objectDataOverflowDroppedObjectCount);
	m_parallelWorkText.content =
		"Parallel Work Units/Workers/Fallback: " +
		String::ToString(frameInfo.parallelCommandWorkUnitCount) + "/" +
		String::ToString(frameInfo.parallelRecordingWorkerCount) + "/" +
		(frameInfo.parallelFallbackReason.empty() ? std::string("None") : frameInfo.parallelFallbackReason);
	m_largeScenePrimitiveText.content =
		"Large Scene Primitives Reg/Static/Dynamic/Unclassified/Slots/Tombstones: " +
		String::ToString(frameInfo.largeScene.registeredPrimitiveCount) + "/" +
		String::ToString(frameInfo.largeScene.staticPrimitiveCount) + "/" +
		String::ToString(frameInfo.largeScene.dynamicPrimitiveCount) + "/" +
		String::ToString(frameInfo.largeScene.unclassifiedPrimitiveCount) + "/" +
		String::ToString(frameInfo.largeScene.allocatedPrimitiveSlotCount) + "/" +
		String::ToString(frameInfo.largeScene.tombstonedPrimitiveSlotCount);
	m_largeSceneVisibilityText.content =
		"Large Scene Visibility SpatialCandidates/FullScanCandidates/Records/Tested/Visible/Meshes: " +
		String::ToString(frameInfo.largeScene.spatialCandidateCount) + "/" +
		String::ToString(frameInfo.largeScene.fullScanCandidateCount) + "/" +
		String::ToString(frameInfo.largeScene.primitiveRecordsTouched) + "/" +
		String::ToString(frameInfo.largeScene.visibilityTestedPrimitiveCount) + "/" +
		String::ToString(frameInfo.largeScene.visiblePrimitiveCount) + "/" +
		String::ToString(frameInfo.largeScene.visibleMeshCount);
	m_largeSceneCullReasonText.content = BuildLargeSceneCullReasonText(frameInfo.largeScene.culledByReason);
	m_largeSceneSyncText.content =
		"Large Scene Sync Touched/FullSweeps/SweepSlots/DirtyBounds/SlotReuse: " +
		String::ToString(frameInfo.largeScene.syncTouchedPrimitiveCount) + "/" +
		String::ToString(frameInfo.largeScene.syncFullSweepCount) + "/" +
		String::ToString(frameInfo.largeScene.syncSweepTouchedSlotCount) + "/" +
		String::ToString(frameInfo.largeScene.boundsDirtyPrimitiveCount) + "/" +
		String::ToString(frameInfo.largeScene.primitiveSlotReuseCount);
	m_largeSceneFinalizationText.content =
		"Large Scene Finalization Prims/Commands/Rebuilds/Raw/Submitted/Groups: " +
		String::ToString(frameInfo.largeScene.finalizationTouchedPrimitiveCount) + "/" +
		String::ToString(frameInfo.largeScene.finalizationTouchedCommandCount) + "/" +
		String::ToString(frameInfo.largeScene.commandOffsetRebuildCount) + "/" +
		String::ToString(frameInfo.largeScene.rawVisibleDrawCount) + "/" +
		String::ToString(frameInfo.largeScene.submittedDrawCount) + "/" +
		String::ToString(frameInfo.largeScene.dynamicInstanceGroupCount);
	m_largeSceneResidencyText.content =
		"Large Scene Residency Dependencies/Tickets/ModeledResidentCPU/ModeledResidentGPU/ModeledRequestedCPU/ModeledRequestedGPU: " +
		String::ToString(frameInfo.largeScene.streamingDependencyCount) + "/" +
		String::ToString(frameInfo.largeScene.residencyTicketCount) + "/" +
		String::ToString(frameInfo.largeScene.residentCpuBytes) + "/" +
		String::ToString(frameInfo.largeScene.residentGpuBytes) + "/" +
		String::ToString(frameInfo.largeScene.requestedCpuBytes) + "/" +
		String::ToString(frameInfo.largeScene.requestedGpuBytes);
	m_largeSceneStreamingText.content =
		"Large Scene Streaming Requests/Commits/Evicts/OcclusionTests/OcclusionCulled/HZBns/Commitns: " +
		String::ToString(frameInfo.largeScene.streamingRequestCount) + "/" +
		String::ToString(frameInfo.largeScene.streamingCommitCount) + "/" +
		String::ToString(frameInfo.largeScene.streamingEvictCount) + "/" +
		String::ToString(frameInfo.largeScene.occlusionTestCount) + "/" +
		String::ToString(frameInfo.largeScene.occlusionCulledCount) + "/" +
		String::ToString(frameInfo.largeScene.hzbBuildTimeNs) + "/" +
		String::ToString(frameInfo.largeScene.streamingCommitTimeNs);
	m_largeSceneHZBHistoryPruneText.content =
		"Large Scene HZB History Prune Touched/RemovedHandles/RemovedKeys/ns: " +
		String::ToString(frameInfo.largeScene.hzbHistoryPruneTouchedHandleCount) + "/" +
		String::ToString(frameInfo.largeScene.hzbHistoryPruneRemovedHandleCount) + "/" +
		String::ToString(frameInfo.largeScene.hzbHistoryPruneRemovedKeyCount) + "/" +
		String::ToString(frameInfo.largeScene.hzbHistoryPruneTimeNs);
	m_largeSceneTimingText.content =
		"Large Scene Timings Sync/SerialVis/ParallelVis/Finalize ns: " +
		String::ToString(frameInfo.largeScene.syncTimeNs) + "/" +
		String::ToString(frameInfo.largeScene.serialVisibilityTimeNs) + "/" +
		String::ToString(frameInfo.largeScene.parallelVisibilityTimeNs) + "/" +
		String::ToString(frameInfo.largeScene.queueFinalizationTimeNs);
	m_gBufferMaterialSyncText.content = "GBuffer Material Syncs: " + String::ToString(frameInfo.gBufferMaterialSyncCount);
	m_gBufferMaterialResolveText.content =
		"GBuffer Material Resolve H/M: " +
		String::ToString(frameInfo.gBufferMaterialResolveHitCount) + "/" +
		String::ToString(frameInfo.gBufferMaterialResolveMissCount);
	m_preparedDrawStaticBaseCacheText.content =
		"Prepared Draw Static Base H/M: " +
		String::ToString(frameInfo.preparedRecordedDrawStaticBaseCacheHitCount) + "/" +
		String::ToString(frameInfo.preparedRecordedDrawStaticBaseCacheMissCount);
	m_bindingSetCreationText.content = "Binding Sets Created: " + String::ToString(frameInfo.renderBindingSetCreationCount);
	m_snapshotBufferCreationText.content = "Snapshot Buffers Created: " + String::ToString(frameInfo.renderSnapshotBufferCreationCount);
	m_framesInFlightText.content = "Frames In Flight: " + String::ToString(frameInfo.inFlightFrameCount);
	m_blockedFramesText.content = "Blocked Frames: " + String::ToString(frameInfo.blockedFrameCount);
	m_publishStateText.content =
		std::string("Publish State: ") + ToFramePublishStateText(frameInfo.publishState);
	m_frameStageText.content =
		std::string("Frame Stage: ") + ToThreadedFrameStageSummaryText(frameInfo.stageSummary);
	m_retirementStateText.content =
		std::string("Retirement State: ") + ToFrameRetirementStateText(frameInfo.retirementState);
	m_rhiSafetyText.content =
		std::string("RHI Safety DeviceLost/UnsafeQuarantine: ") +
		(frameInfo.deviceLostDetected ? "Yes" : "No") + "/" +
		(frameInfo.unsafeGpuWorkQuarantined ? "Yes" : "No");
}
