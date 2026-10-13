#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Rendering/Context/RenderScenePackage.h"
#include "RenderDef.h"

namespace NLS::Render::Context
{
    struct NLS_RENDER_API RhiSubmissionFrame
    {
        uint64_t frameId = 0u;
        uint64_t uiOverlaySnapshotFrameId = 0u;
        uint32_t frameContextIndex = 0u;
        bool retirementFenceWaited = false;
        bool offscreenOnly = false;
        bool usedExternalOutputBridge = false;
        bool recordedVisibleWork = false;
        uint64_t recordedDrawCount = 0u;
        uint64_t recordedPassCount = 0u;
        uint64_t recordedWorkUnitCount = 0u;
        RHI::RHICommandListSubmissionMetadata commandListSubmission;
        std::vector<ParallelDrawCommandBatchMetadata> parallelDrawCommandBatches;
        uint32_t parallelRecordingWorkerCount = 0u;
        uint64_t translatedWorkUnitCount = 0u;
        std::string parallelFallbackReason;
        bool usedParallelCommandPath = false;
        bool usedTranslationMerge = false;
        bool usedSerialCommandPath = false;
        bool usedResourceStateTracker = false;
        uint64_t transientBufferRegistrationCount = 0u;
        uint64_t transientTextureRegistrationCount = 0u;
        uint64_t retiredTransientBufferCount = 0u;
        uint64_t retiredTransientTextureCount = 0u;
        bool usedDescriptorAllocator = false;
        uint64_t descriptorTransientUsed = 0u;
        uint64_t descriptorTransientPeak = 0u;
        uint64_t descriptorTransientRetired = 0u;
        bool deferredFrameScopedRetirement = false;
        uint64_t descriptorPersistentUsed = 0u;
        uint64_t descriptorPersistentReleased = 0u;
        uint64_t descriptorAllocationFailures = 0u;
        uint64_t asyncComputeCandidateWorkloadCount = 0u;
        uint64_t externalOutputTextureCount = 0u;
        bool asyncComputeQueueAvailable = false;
        bool usedAsyncComputeQueueSubmission = false;
        AsyncComputeDisposition asyncComputeDisposition = AsyncComputeDisposition::NotRequested;
        bool pipelineMainlineActive = false;
        uint64_t pipelineBypassCount = 0u;
        uint64_t pipelineCacheGraphicsHits = 0u;
        uint64_t pipelineCacheGraphicsMisses = 0u;
        uint64_t pipelineCacheGraphicsStores = 0u;
        uint64_t pipelineCacheGraphicsEntries = 0u;
        uint64_t pipelineCacheComputeHits = 0u;
        uint64_t pipelineCacheComputeMisses = 0u;
        uint64_t pipelineCacheComputeStores = 0u;
        uint64_t pipelineCacheComputeEntries = 0u;
        uint64_t queueOperationFailureCount = 0u;
        std::string lastQueueOperationFailure;
        uint64_t currentFrameQueueOperationFailureCount = 0u;
        std::string currentFrameLastQueueOperationFailure;
        uint64_t commandRecordingFailureCount = 0u;
        std::string lastCommandRecordingFailure;
        bool deviceLostDetected = false;
        std::string deviceLostReason;
        bool unsafeGpuWorkQuarantined = false;
        std::string unsafeGpuWorkQuarantineReason;
        bool submittedSuccessfully = true;
            /**
             * True when the frame never rendered because the swapchain it was
             * published against stopped matching the surface (a synchronous window
             * resize, or a surface-driven recreation). The frame is a clean drop, not
             * a defect: the swapchain is rebuilt and the next frame renders normally.
             * Retiring it as failed would make a routine resize look like an
             * unrecoverable rendering error.
             */
            bool droppedForSwapchainRecreation = false;
    };

    enum class RenderSceneAttribution : uint8_t
    {
        Unknown = 0,
        RendererPrepared,
        Failed
    };

    enum class RhiSubmissionAttribution : uint8_t
    {
        Unknown = 0,
        Worker,
        SynchronousDrain
    };

    enum class ThreadedFramePublishOrigin : uint8_t
    {
        Unknown = 0,
        PreparedBuilder
    };

    struct NLS_RENDER_API ThreadedFrameTelemetry
    {
        uint64_t inFlightFrameCount = 0u;
        uint64_t blockedPublishCount = 0u;
        uint64_t reservedSlotWaitCount = 0u;
        uint64_t reservedSlotWaitTimeoutCount = 0u;
        uint64_t reservedSlotWaitTotalNs = 0u;
        uint64_t reservedSlotWaitMaxNs = 0u;
        uint64_t publishedFrameCount = 0u;
        uint64_t latestPublishedFrameId = 0u;
        uint64_t latestRetiredFrameId = 0u;
        uint64_t latestFailedRetiredFrameId = 0u;
        uint64_t successfullyRetiredFrameCount = 0u;
        uint64_t failedRetiredFrameCount = 0u;
        uint64_t droppedForSwapchainRecreationFrameCount = 0u;
        uint64_t latestDroppedForSwapchainRecreationFrameId = 0u;
        Data::FramePublishState publishState = Data::FramePublishState::Direct;
        Data::FrameRetirementState retirementState = Data::FrameRetirementState::Direct;
        ThreadedFramePublishOrigin publishOrigin = ThreadedFramePublishOrigin::Unknown;
        RenderSceneAttribution renderSceneAttribution = RenderSceneAttribution::Unknown;
        RhiSubmissionAttribution rhiSubmissionAttribution = RhiSubmissionAttribution::Unknown;
        bool descriptorMainlineActive = false;
        bool pipelineMainlineActive = false;
        bool transientLifetimeMainlineActive = false;
        bool retirementMainlineActive = false;
        uint64_t descriptorBypassCount = 0u;
        uint64_t pipelineBypassCount = 0u;
        uint64_t transientLifetimeBypassCount = 0u;
        uint64_t retirementBypassCount = 0u;
        uint64_t transientTextureRegistrationCount = 0u;
        uint64_t transientBufferRegistrationCount = 0u;
        uint64_t retiredTransientTextureCount = 0u;
        uint64_t retiredTransientBufferCount = 0u;
        uint64_t descriptorTransientPeak = 0u;
        uint64_t descriptorAllocationFailures = 0u;
        uint64_t pipelineCacheGraphicsHits = 0u;
        uint64_t pipelineCacheGraphicsMisses = 0u;
        uint64_t pipelineCacheGraphicsStores = 0u;
        uint64_t pipelineCacheGraphicsEntries = 0u;
        uint64_t pipelineCacheComputeHits = 0u;
        uint64_t pipelineCacheComputeMisses = 0u;
        uint64_t pipelineCacheComputeStores = 0u;
        uint64_t pipelineCacheComputeEntries = 0u;
        uint64_t parallelCommandWorkUnitCount = 0u;
        uint32_t parallelRecordingWorkerCount = 0u;
        std::string parallelFallbackReason;
        uint64_t queueOperationFailureCount = 0u;
        std::string lastQueueOperationFailure;
        uint64_t currentFrameQueueOperationFailureCount = 0u;
        std::string currentFrameLastQueueOperationFailure;
        bool deviceLostDetected = false;
        std::string deviceLostReason;
        bool unsafeGpuWorkQuarantined = false;
        std::string unsafeGpuWorkQuarantineReason;
    };

}

