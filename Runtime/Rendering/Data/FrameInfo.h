#pragma once

#include <cstdint>
#include "RenderDef.h"
namespace NLS::Render::Data
{
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
	};
}
