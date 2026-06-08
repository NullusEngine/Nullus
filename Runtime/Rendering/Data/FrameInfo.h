#pragma once

#include <array>
#include <cstdint>
#include <string>
#include "RenderDef.h"
namespace NLS::Render::Data
{
	static constexpr size_t kLargeSceneCullReasonCount = 16u;
	static constexpr size_t kLargeSceneLodSelectionBucketCount = 8u;
	static constexpr size_t kLargeSceneTelemetryScalarFieldCount = 54u;
	static constexpr size_t kLargeSceneTelemetryExpectedSize =
		(kLargeSceneTelemetryScalarFieldCount +
			kLargeSceneCullReasonCount +
			kLargeSceneLodSelectionBucketCount) * sizeof(uint64_t);

	enum class FramePublishState : uint8_t
	{
		Direct = 0,
		Open,
		BackPressured
	};

	enum class ThreadedFrameStageSummary : uint8_t
	{
		Direct = 0,
		Logic,
		RenderScene,
		Rhi,
		Retired
	};

	enum class FrameRetirementState : uint8_t
	{
		Direct = 0,
		Pending,
		Ready,
		Consumed
	};

	struct NLS_RENDER_API LargeSceneTelemetry
	{
		uint64_t registeredPrimitiveCount = 0;
		uint64_t staticPrimitiveCount = 0;
		uint64_t dynamicPrimitiveCount = 0;
		uint64_t unclassifiedPrimitiveCount = 0;
		uint64_t spatialCandidateCount = 0;
		uint64_t fullScanCandidateCount = 0;
		uint64_t visiblePrimitiveCount = 0;
		uint64_t visibleMeshCount = 0;
		std::array<uint64_t, kLargeSceneCullReasonCount> culledByReason {};
		std::array<uint64_t, kLargeSceneLodSelectionBucketCount> lodSelectionCount {};
		uint64_t activeHLODClusterCount = 0;
		uint64_t occlusionTestCount = 0;
		uint64_t occlusionCulledCount = 0;
		uint64_t streamingRequestCount = 0;
		uint64_t streamingCommitCount = 0;
		uint64_t streamingEvictCount = 0;
		uint64_t streamingDependencyCount = 0;
		uint64_t residencyTicketCount = 0;
		uint64_t residentCpuBytes = 0;
		uint64_t residentGpuBytes = 0;
		uint64_t requestedCpuBytes = 0;
		uint64_t requestedGpuBytes = 0;
		uint64_t primitiveRecordsTouched = 0;
		uint64_t allocatedPrimitiveSlotCount = 0;
		uint64_t tombstonedPrimitiveSlotCount = 0;
		uint64_t syncSweepTouchedSlotCount = 0;
		uint64_t syncTouchedPrimitiveCount = 0;
		uint64_t syncFullSweepCount = 0;
		uint64_t boundsDirtyPrimitiveCount = 0;
		uint64_t primitiveSlotReuseCount = 0;
		uint64_t visibilityTestedPrimitiveCount = 0;
		uint64_t visibilityBitsetWordCount = 0;
		uint64_t finalizationTouchedPrimitiveCount = 0;
		uint64_t finalizationTouchedCommandCount = 0;
		uint64_t commandOffsetRebuildCount = 0;
		uint64_t rawVisibleDrawCount = 0;
		uint64_t submittedDrawCount = 0;
		uint64_t dynamicInstanceGroupCount = 0;
		uint64_t dynamicCandidateCount = 0;
		uint64_t dynamicRecordsTouched = 0;
		uint64_t staticIndexRefitCount = 0;
		uint64_t staticIndexRebuildCount = 0;
		uint64_t staticIndexLastGoodQueryCount = 0;
		uint64_t staticIndexDirtyOverlayCount = 0;
		uint64_t spatialRebuildFallbackCount = 0;
		uint64_t dynamicIndexUpdateCount = 0;
		uint64_t syncTimeNs = 0;
		uint64_t serialVisibilityTimeNs = 0;
		uint64_t parallelVisibilityTimeNs = 0;
		uint64_t queueFinalizationTimeNs = 0;
		uint64_t hzbBuildTimeNs = 0;
		uint64_t hzbHistoryPruneTouchedHandleCount = 0;
		uint64_t hzbHistoryPruneRemovedHandleCount = 0;
		uint64_t hzbHistoryPruneRemovedKeyCount = 0;
		uint64_t hzbHistoryPruneTimeNs = 0;
		uint64_t streamingCommitTimeNs = 0;
	};
	static_assert(
		sizeof(LargeSceneTelemetry) == kLargeSceneTelemetryExpectedSize,
		"Update large-scene telemetry aggregation, formatting, and tests when fields change.");

	/**
	* Holds information about a given frame
	*/
	struct NLS_RENDER_API FrameInfo
	{
		uint64_t batchCount = 0;
		uint64_t instanceCount = 0;
		uint64_t polyCount = 0;
		uint64_t vertexCount = 0;
		uint64_t inFlightFrameCount = 0;
		uint64_t blockedFrameCount = 0;
        uint64_t reservedSlotWaitCount = 0;
        uint64_t reservedSlotWaitTimeoutCount = 0;
        uint64_t reservedSlotWaitTotalNs = 0;
		FramePublishState publishState = FramePublishState::Direct;
		ThreadedFrameStageSummary stageSummary = ThreadedFrameStageSummary::Direct;
		FrameRetirementState retirementState = FrameRetirementState::Direct;
        bool descriptorMainlineActive = false;
        bool pipelineMainlineActive = false;
        bool transientLifetimeMainlineActive = false;
        bool retirementMainlineActive = false;
        uint64_t descriptorBypassCount = 0;
        uint64_t pipelineBypassCount = 0;
        uint64_t transientLifetimeBypassCount = 0;
        uint64_t retirementBypassCount = 0;
        uint64_t transientTextureRegistrationCount = 0;
        uint64_t transientBufferRegistrationCount = 0;
        uint64_t retiredTransientTextureCount = 0;
        uint64_t retiredTransientBufferCount = 0;
        uint64_t descriptorTransientPeak = 0;
        uint64_t descriptorAllocationFailures = 0;
        uint64_t pipelineCacheGraphicsHits = 0;
        uint64_t pipelineCacheGraphicsMisses = 0;
        uint64_t pipelineCacheGraphicsStores = 0;
        uint64_t pipelineCacheGraphicsEntries = 0;
        uint64_t pipelineCacheComputeHits = 0;
        uint64_t pipelineCacheComputeMisses = 0;
        uint64_t pipelineCacheComputeStores = 0;
        uint64_t pipelineCacheComputeEntries = 0;
        uint64_t parseSceneCallCount = 0;
        uint64_t parsedOpaqueDrawableCount = 0;
        uint64_t parsedTransparentDrawableCount = 0;
        uint64_t parsedSkyboxDrawableCount = 0;
        uint64_t gBufferMaterialSyncCount = 0;
        uint64_t gBufferMaterialResolveHitCount = 0;
        uint64_t gBufferMaterialResolveMissCount = 0;
        uint64_t preparedRecordedDrawStaticBaseCacheHitCount = 0;
        uint64_t preparedRecordedDrawStaticBaseCacheMissCount = 0;
        uint64_t renderBindingSetCreationCount = 0;
        uint64_t renderSnapshotBufferCreationCount = 0;
        uint64_t rawVisibleObjectCount = 0;
        uint64_t submittedSceneDrawCount = 0;
        uint64_t dynamicInstanceGroupCount = 0;
        uint64_t largestInstanceGroupSize = 0;
        uint64_t cachedCommandRebuildCount = 0;
        uint64_t objectDataOverflowDroppedObjectCount = 0;
        uint64_t parallelCommandWorkUnitCount = 0;
        uint64_t parallelRecordingWorkerCount = 0;
        std::string parallelFallbackReason;
        bool deviceLostDetected = false;
        std::string deviceLostReason;
        bool unsafeGpuWorkQuarantined = false;
        std::string unsafeGpuWorkQuarantineReason;
		LargeSceneTelemetry largeScene;
	};
}
