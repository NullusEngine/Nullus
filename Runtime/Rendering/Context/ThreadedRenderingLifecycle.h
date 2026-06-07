#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Math/Vector4.h"
#include "Rendering/Context/RenderFrameBuild.h"
#include "Rendering/Context/RenderFrameInput.h"
#include "Rendering/Core/RenderClearValues.h"
#include "Rendering/Data/FrameInfo.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "RenderDef.h"

namespace NLS::Render::RHI
{
    class RHIBuffer;
    class RHIGraphicsPipeline;
    class RHIComputePipeline;
    class RHIBindingSet;
    class RHIMesh;
    class RHITexture;
    class RHITextureView;
}

namespace NLS::Render::Context
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    struct ThreadedRenderingLifecycleTestAccess;
#endif

    enum class RenderPassCommandKind : uint8_t
    {
        Opaque = 0,
        Transparent,
        Skybox,
        Helper,
        GBuffer,
        Lighting,
        Compute,
        Decal
    };

    struct NLS_RENDER_API RecordedDrawCommandInput
    {
        std::shared_ptr<RHI::RHIGraphicsPipeline> pipeline;
        std::shared_ptr<RHI::RHIBindingSet> frameBindingSet;
        std::shared_ptr<RHI::RHIBindingSet> objectBindingSet;
        std::shared_ptr<RHI::RHIBindingSet> passBindingSet;
        std::shared_ptr<RHI::RHIBindingSet> materialBindingSet;
        std::shared_ptr<RHI::RHIMesh> mesh;
        uint32_t instanceCount = 0u;
        uint32_t vertexStart = 0u;
        uint32_t vertexCount = 0u;
        uint32_t objectIndex = 0u;
        bool usesObjectIndex = false;
    };

    struct NLS_RENDER_API RecordedComputeBindingSetInput
    {
        uint32_t setIndex = 0u;
        std::shared_ptr<RHI::RHIBindingSet> bindingSet;
    };

    struct NLS_RENDER_API RecordedComputeDispatchInput
    {
        std::string debugName;
        std::shared_ptr<RHI::RHIComputePipeline> pipeline;
        std::vector<RecordedComputeBindingSetInput> bindingSets;
        uint32_t groupCountX = 1u;
        uint32_t groupCountY = 1u;
        uint32_t groupCountZ = 1u;
        std::vector<std::shared_ptr<RHI::RHIBuffer>> shaderReadBuffersBefore;
        std::vector<std::shared_ptr<RHI::RHIBuffer>> shaderWriteBuffersBefore;
        std::vector<std::shared_ptr<RHI::RHIBuffer>> uavBarrierBuffersBefore;
        std::vector<std::shared_ptr<RHI::RHIBuffer>> uavBarrierBuffersAfter;
        std::vector<std::shared_ptr<RHI::RHIBuffer>> shaderReadBuffersAfter;
    };

    enum class QueueDependencyPolicy : uint8_t
    {
        None = 0,
        Previous,
        LastGraphics,
        LastCompute
    };

    struct NLS_RENDER_API BufferVisibilityTransition
    {
        std::shared_ptr<RHI::RHIBuffer> buffer;
        RHI::ResourceState before = RHI::ResourceState::Unknown;
        RHI::ResourceState after = RHI::ResourceState::Unknown;
        RHI::PipelineStageMask sourceStages = RHI::PipelineStageMask::None;
        RHI::PipelineStageMask destinationStages = RHI::PipelineStageMask::None;
        RHI::AccessMask sourceAccess = RHI::AccessMask::None;
        RHI::AccessMask destinationAccess = RHI::AccessMask::None;
    };

    struct NLS_RENDER_API TextureVisibilityTransition
    {
        std::shared_ptr<RHI::RHITexture> texture;
        RHI::RHISubresourceRange subresourceRange{};
        RHI::ResourceState before = RHI::ResourceState::Unknown;
        RHI::ResourceState after = RHI::ResourceState::Unknown;
        RHI::PipelineStageMask sourceStages = RHI::PipelineStageMask::None;
        RHI::PipelineStageMask destinationStages = RHI::PipelineStageMask::None;
        RHI::AccessMask sourceAccess = RHI::AccessMask::None;
        RHI::AccessMask destinationAccess = RHI::AccessMask::None;
    };

    enum class ResourceAccessMode : uint8_t
    {
        Read = 0,
        Write
    };

    struct NLS_RENDER_API BufferResourceAccess
    {
        std::shared_ptr<RHI::RHIBuffer> buffer;
        ResourceAccessMode mode = ResourceAccessMode::Read;
        RHI::ResourceState state = RHI::ResourceState::Unknown;
        RHI::PipelineStageMask stages = RHI::PipelineStageMask::None;
        RHI::AccessMask access = RHI::AccessMask::None;
    };

    struct NLS_RENDER_API TextureResourceAccess
    {
        std::shared_ptr<RHI::RHITexture> texture;
        RHI::RHISubresourceRange subresourceRange{};
        ResourceAccessMode mode = ResourceAccessMode::Read;
        RHI::ResourceState state = RHI::ResourceState::Unknown;
        RHI::PipelineStageMask stages = RHI::PipelineStageMask::None;
        RHI::AccessMask access = RHI::AccessMask::None;
    };

    enum class ThreadedDependencyKind : uint8_t
    {
        ResourceVisibility = 0,
        QueueSynchronization
    };

    enum class ThreadedDependencyResourceKind : uint8_t
    {
        None = 0,
        Buffer,
        Texture
    };

    struct NLS_RENDER_API WorkUnitDependencyEdge
    {
        uint64_t sourceWorkUnitIndex = 0u;
        uint64_t targetWorkUnitIndex = 0u;
        ThreadedDependencyKind kind = ThreadedDependencyKind::ResourceVisibility;
        ThreadedDependencyResourceKind resourceKind = ThreadedDependencyResourceKind::None;
        std::optional<BufferResourceAccess> sourceBufferAccess;
        std::optional<BufferResourceAccess> targetBufferAccess;
        std::optional<TextureResourceAccess> sourceTextureAccess;
        std::optional<TextureResourceAccess> targetTextureAccess;
    };

    struct NLS_RENDER_API RenderPassCommandInput
    {
        RenderPassCommandKind kind = RenderPassCommandKind::Opaque;
        RHI::QueueType queueType = RHI::QueueType::Graphics;
        QueueDependencyPolicy queueDependencyPolicy = QueueDependencyPolicy::Previous;
        std::optional<uint64_t> dependencySourceWorkUnitIndex;
        bool requiresDependencyVisibility = false;
        std::string debugName;
        uint64_t drawCount = 0u;
        std::vector<RecordedDrawCommandInput> recordedDrawCommands;
        std::vector<RecordedComputeDispatchInput> computeDispatchInputs;
        bool requiresFrameData = false;
        bool requiresObjectData = false;
        bool requiresLightingData = false;
        bool targetsSwapchain = false;
        uint32_t renderWidth = 0u;
        uint32_t renderHeight = 0u;
        Maths::Vector4 clearColorValue = Core::DefaultOpaqueClearColor();
        bool clearColor = false;
        bool clearDepth = false;
        bool clearStencil = false;
        bool usesColorAttachment = false;
        bool usesDepthStencilAttachment = false;
        bool writesDepthStencilAttachment = false;
        std::vector<BufferResourceAccess> bufferResourceAccesses;
        std::vector<TextureResourceAccess> textureResourceAccesses;
        std::vector<BufferVisibilityTransition> bufferVisibilityTransitions;
        std::vector<TextureVisibilityTransition> textureVisibilityTransitions;
        std::vector<BufferVisibilityTransition> exportedBufferVisibilityTransitions;
        std::vector<TextureVisibilityTransition> exportedTextureVisibilityTransitions;
        std::vector<std::shared_ptr<RHI::RHITexture>> gbufferTextures;
        std::vector<std::shared_ptr<RHI::RHITextureView>> colorAttachmentViews;
        std::shared_ptr<RHI::RHITextureView> depthStencilAttachmentView;
    };

    inline constexpr uint64_t kRecordedDrawCommandSliceThreshold = 512u;
    inline constexpr uint64_t kInvalidParallelCommandSourcePassIndex = (std::numeric_limits<uint64_t>::max)();

    struct NLS_RENDER_API ParallelCommandWorkUnit
    {
        uint64_t workUnitIndex = 0u;
        uint64_t submissionOrder = 0u;
        std::string debugName;
        RenderPassCommandInput commandInput;
        std::vector<WorkUnitDependencyEdge> incomingDependencyEdges;
        bool eligibleForParallelRecording = false;
        bool eligibleForParallelTranslation = false;
        uint64_t sourcePassIndex = kInvalidParallelCommandSourcePassIndex;
        uint32_t sliceIndex = 0u;
        uint32_t sliceCount = 1u;
        uint64_t recordedDrawBegin = 0u;
        uint64_t recordedDrawCount = 0u;
        bool requiresOrderedSlicedSubmission = false;
        bool usesInRenderPassChildCommandRecording = false;
    };

    enum class ParallelDrawCommandPassRole : uint8_t
    {
        Opaque = 0,
        Transparent,
        Skybox,
        Helper,
        Auxiliary,
        Compute,
        Decal
    };

    struct NLS_RENDER_API ParallelDrawCommandBatchMetadata
    {
        ParallelDrawCommandPassRole passRole = ParallelDrawCommandPassRole::Auxiliary;
        uint64_t workUnitIndex = 0u;
        uint64_t submissionOrder = 0u;
        std::string debugName;
        RHI::QueueType queueType = RHI::QueueType::Graphics;
        uint64_t drawCommandCount = 0u;
        uint64_t recordedDrawCommandCount = 0u;
        bool eligibleForParallelRecording = false;
        bool eligibleForParallelTranslation = false;
        std::vector<WorkUnitDependencyEdge> incomingDependencyEdges;
        uint64_t sourcePassIndex = kInvalidParallelCommandSourcePassIndex;
        uint32_t sliceIndex = 0u;
        uint32_t sliceCount = 1u;
        uint64_t recordedDrawBegin = 0u;
        uint64_t recordedDrawCount = 0u;
        bool requiresOrderedSlicedSubmission = false;
        bool usesInRenderPassChildCommandRecording = false;
    };

    NLS_RENDER_API bool IsRenderPassEligibleForParallelRecording(const RenderPassCommandInput& passInput);
    NLS_RENDER_API bool CanRenderPassUseRecordedDrawCommandSlices(const RenderPassCommandInput& passInput);
    NLS_RENDER_API std::vector<ParallelDrawCommandBatchMetadata> BuildParallelDrawCommandBatchMetadata(
        const std::vector<ParallelCommandWorkUnit>& workUnits);
    NLS_RENDER_API std::vector<ParallelCommandWorkUnit> BuildRecordedDrawCommandWorkUnitsForPass(
        const RenderPassCommandInput& passInput,
        uint64_t sourcePassIndex,
        uint64_t workUnitBaseIndex);

    enum class AsyncComputeDisposition : uint8_t
    {
        NotRequested = 0,
        DisabledNoCapability,
        DisabledNoComputeQueue,
        DisabledNoEligibleWorkload,
        ReadyButNotScheduled,
        Submitted
    };

    struct NLS_RENDER_API FrameSnapshot
    {
        uint64_t frameId = 0u;
        uint64_t sceneRevision = 0u;
        uint32_t renderWidth = 0u;
        uint32_t renderHeight = 0u;
        bool targetsSwapchain = true;
        bool hasExternalOutput = false;
        Maths::Vector4 clearColor = Core::DefaultOpaqueClearColor();
        bool clearColorBuffer = true;
        bool clearDepthBuffer = true;
        bool clearStencilBuffer = true;
        bool hasGeometryFrustum = false;
        bool hasLightFrustum = false;
        bool hasSceneInput = false;
        uint64_t sceneGameObjectCount = 0u;
        uint64_t sceneModelRendererCount = 0u;
        uint64_t sceneLightCount = 0u;
        uint64_t sceneSkyboxCount = 0u;
        uint64_t visibleOpaqueDrawCount = 0u;
        uint64_t visibleDecalDrawCount = 0u;
        uint64_t visibleTransparentDrawCount = 0u;
        uint64_t visibleSkyboxDrawCount = 0u;
        uint64_t visibleHelperDrawCount = 0u;
        uint64_t externalOutputIdentity = 0u;
        std::vector<uint64_t> externalOutputIdentities;
        uint32_t externalOutputTextureCount = 0u;
        std::vector<RecordedDrawCommandInput> recordedDrawCommands;
    };

    struct NLS_RENDER_API RenderScenePackage
    {
        uint64_t frameId = 0u;
        bool targetsSwapchain = true;
        bool hasVisibleDraws = false;
        bool hasLightingData = false;
        bool frameDataReady = false;
        bool objectDataReady = false;
        bool lightingDataReady = false;
        Maths::Vector4 clearColorValue = Core::DefaultOpaqueClearColor();
        bool clearColorBuffer = true;
        bool clearDepthBuffer = true;
        bool clearStencilBuffer = true;
        uint32_t renderWidth = 0u;
        uint32_t renderHeight = 0u;
        uint64_t sceneGameObjectCount = 0u;
        uint64_t visibleDrawCount = 0u;
        uint64_t opaqueDrawCount = 0u;
        uint64_t decalDrawCount = 0u;
        uint64_t transparentDrawCount = 0u;
        uint64_t skyboxDrawCount = 0u;
        uint64_t helperDrawCount = 0u;
        bool hasOpaquePass = false;
        bool hasDecalPass = false;
        bool hasTransparentPass = false;
        bool hasSkyboxPass = false;
        bool hasHelperPass = false;
        uint64_t passPlanCount = 0u;
        uint64_t drawCommandCount = 0u;
        uint64_t materialBatchCount = 0u;
        uint64_t renderTargetUseCount = 0u;
        bool containsCommandInputs = false;
        std::vector<RenderPassCommandInput> passCommandInputs;
        bool containsParallelCommandWorkUnits = false;
        uint64_t parallelCommandWorkUnitCount = 0u;
        std::vector<ParallelCommandWorkUnit> parallelCommandWorkUnits;
        std::vector<ParallelDrawCommandBatchMetadata> parallelDrawCommandBatches;
        std::vector<WorkUnitDependencyEdge> workUnitDependencyEdges;
        bool hasAsyncComputeWorkload = false;
        uint64_t asyncComputeWorkloadCount = 0u;
        bool containsComputeDispatchInputs = false;
        std::vector<RecordedComputeDispatchInput> computeDispatchInputs;
        std::vector<RecordedDrawCommandInput> recordedDrawCommands;
        std::vector<std::shared_ptr<RHI::RHITexture>> extractedTextures;
        std::shared_ptr<RHI::RHITexture> preferredReadbackTexture;
        uint64_t externalSceneOutputIdentity = 0u;
        std::vector<uint64_t> externalSceneOutputIdentities;
        uint32_t externalSceneOutputTextureCount = 0u;
    };

    using PreparedRenderSceneBuilder = std::function<RenderScenePackage()>;

    struct NLS_RENDER_API RhiSubmissionFrame
    {
        uint64_t frameId = 0u;
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
    };

    enum class ThreadedFrameStage : uint8_t
    {
        Available = 0,
        Published = 1,
        RenderScenePreparing = 2,
        RenderReady = 3,
        RhiSubmitting = 4,
        Retired = 5,
        RenderSceneResolving = 6
    };

    enum class RenderSceneAttribution : uint8_t
    {
        Unknown = 0,
        RendererPrepared,
        PreparedBuilderMissing,
        SnapshotHarness
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
        SnapshotHarness,
        PreparedPackage,
        PreparedBuilder
    };

    struct NLS_RENDER_API InFlightFrameSlot
    {
        size_t slotIndex = 0u;
        ThreadedFrameStage stage = ThreadedFrameStage::Available;
        ThreadedFramePublishOrigin publishOrigin = ThreadedFramePublishOrigin::Unknown;
        RenderSceneAttribution renderSceneAttribution = RenderSceneAttribution::Unknown;
        RhiSubmissionAttribution rhiSubmissionAttribution = RhiSubmissionAttribution::Unknown;
        std::optional<RenderFrameInput> renderFrameInput;
        std::optional<RenderFrameBuild> renderFrameBuild;
        std::optional<FrameSnapshot> snapshot;
        std::optional<PreparedRenderSceneBuilder> preparedRenderSceneBuilder;
        std::optional<RenderScenePackage> renderScenePackage;
        std::optional<RhiSubmissionFrame> submissionFrame;
    };

    struct NLS_RENDER_API ThreadedFrameTelemetry
    {
        uint64_t inFlightFrameCount = 0u;
        uint64_t blockedPublishCount = 0u;
        uint64_t reservedSlotWaitCount = 0u;
        uint64_t reservedSlotWaitTimeoutCount = 0u;
        uint64_t reservedSlotWaitTotalNs = 0u;
        uint64_t publishedFrameCount = 0u;
        uint64_t latestPublishedFrameId = 0u;
        uint64_t latestRetiredFrameId = 0u;
        uint64_t latestFailedRetiredFrameId = 0u;
        Data::FramePublishState publishState = Data::FramePublishState::Direct;
        Data::ThreadedFrameStageSummary stageSummary = Data::ThreadedFrameStageSummary::Direct;
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

    struct NLS_RENDER_API RenderScenePreparingResolutionDesc
    {
        std::function<RenderScenePackage(const FrameSnapshot& snapshot)> buildSnapshotHarnessRenderScenePackage;
        std::function<RenderScenePackage(const FrameSnapshot& snapshot)> buildPreparedBuilderMissingRenderScenePackage;
    };

    class NLS_RENDER_API ThreadedRenderingLifecycle
    {
    public:
        explicit ThreadedRenderingLifecycle(uint32_t slotCount);

        bool TryPublishFrameSnapshot(const FrameSnapshot& snapshot, size_t* publishedSlotIndex = nullptr);
        bool TryPublishPreparedFrame(
            const FrameSnapshot& snapshot,
            const RenderScenePackage& renderScenePackage,
            size_t* publishedSlotIndex = nullptr);
        bool TryPublishPreparedFrameBuilder(
            const FrameSnapshot& snapshot,
            PreparedRenderSceneBuilder renderSceneBuilder,
            size_t* publishedSlotIndex = nullptr);
        bool PublishPreparedFrameBuilder(
            const FrameSnapshot& snapshot,
            PreparedRenderSceneBuilder renderSceneBuilder,
            std::chrono::nanoseconds retirementWaitTimeout,
            size_t* publishedSlotIndex = nullptr);
        bool PublishFrameSnapshot(
            const FrameSnapshot& snapshot,
            std::chrono::nanoseconds retirementWaitTimeout,
            size_t* publishedSlotIndex = nullptr);
        bool TryBeginNextRenderFrameBuild(size_t* slotIndex, RenderFrameInput* renderFrameInput);
        bool TryBeginNextRenderScene(size_t* slotIndex, FrameSnapshot* snapshot);
        bool TryBeginRenderScene(size_t slotIndex);
        bool CompleteRenderScene(
            size_t slotIndex,
            const RenderScenePackage& renderScenePackage,
            RenderSceneAttribution attribution = RenderSceneAttribution::Unknown);
        bool ResolveRenderScenePreparing(
            size_t slotIndex,
            const RenderScenePreparingResolutionDesc& resolutionDesc);
        bool TryBeginNextRhiFrameExecution(size_t* slotIndex, RenderFrameBuild* renderFrameBuild);
        bool TryBeginNextRhiSubmission(size_t* slotIndex, RenderScenePackage* renderScenePackage);
        bool TryBeginRhiSubmission(size_t slotIndex);
        bool CompleteRhiSubmission(
            size_t slotIndex,
            const RhiSubmissionFrame& submissionFrame,
            RhiSubmissionAttribution attribution = RhiSubmissionAttribution::Unknown);
        bool RetireFrame(size_t slotIndex);

        size_t GetSlotCount() const;
        size_t GetInFlightDepth() const;
        bool IsBackPressured() const;
        uint64_t GetBlockedPublishCount() const;
        uint64_t GetPublishedFrameCount() const;
        std::optional<size_t> ReserveReusableSlotIndex(
            std::chrono::nanoseconds retirementWaitTimeout = std::chrono::nanoseconds::zero());
        std::optional<size_t> ReserveReusableSlotIndexExcluding(
            const std::vector<size_t>& excludedSlotIndices,
            std::chrono::nanoseconds retirementWaitTimeout = std::chrono::nanoseconds::zero());
        bool ReleaseReservedReusableSlotIndex(size_t slotIndex);
        std::optional<size_t> GetReservedReusableSlotIndex() const;
        const InFlightFrameSlot* PeekSlot(size_t slotIndex) const;
        std::optional<InFlightFrameSlot> CopySlot(size_t slotIndex) const;
        std::vector<InFlightFrameSlot> CopySlots() const;
        ThreadedFrameTelemetry GetTelemetry() const;
        std::optional<ThreadedFrameTelemetry> TryGetTelemetry() const;
        void ReleaseRetainedFrameResources();

    private:
#if defined(NLS_ENABLE_TEST_HOOKS)
        friend struct ThreadedRenderingLifecycleTestAccess;
#endif

        bool PublishFrameSnapshotLocked(const FrameSnapshot& snapshot, size_t* publishedSlotIndex);
        bool PublishPreparedFrameLocked(
            const FrameSnapshot& snapshot,
            const RenderScenePackage& renderScenePackage,
            size_t* publishedSlotIndex);
        bool PublishPreparedFrameBuilderLocked(
            const FrameSnapshot& snapshot,
            PreparedRenderSceneBuilder renderSceneBuilder,
            size_t* publishedSlotIndex);
        bool CompleteRenderSceneLocked(
            size_t slotIndex,
            const RenderScenePackage& renderScenePackage,
            RenderSceneAttribution attribution,
            ThreadedFrameStage expectedStage);
        bool RetireStaleExternalOutputPublishedFramesLocked();
        bool RetireStaleExternalOutputReadyFramesLocked();
        InFlightFrameSlot* FindReusableSlotLocked(bool allowReservedSlot = false);
        const InFlightFrameSlot* FindReusableSlotReadOnlyLocked(bool allowReservedSlot = false) const;
        const InFlightFrameSlot* FindReservableSlotReadOnlyLocked(
            const std::vector<size_t>& excludedSlotIndices = {}) const;
        bool IsSlotReusableForPublicationLocked(const InFlightFrameSlot& slot, bool reservedCandidate) const;
        bool IsSlotReservedLocked(size_t slotIndex) const;
        bool IsSlotExcludedFromReservationLocked(size_t slotIndex, const std::vector<size_t>& excludedSlotIndices) const;
        void ClearReservedSlotLocked(size_t slotIndex);
        const InFlightFrameSlot* PeekSlotLocked(size_t slotIndex) const;
        void RefreshTelemetryLocked();

        mutable std::mutex m_mutex;
        std::condition_variable m_slotAvailable;
        std::vector<InFlightFrameSlot> m_slots;
        ThreadedFrameTelemetry m_telemetry;
        std::optional<size_t> m_reservedReusableSlotIndex;
        uint64_t m_latestPublishedFrameId = 0u;
        uint64_t m_latestRetiredFrameId = 0u;
        uint64_t m_latestFailedRetiredFrameId = 0u;
        std::unordered_map<uint64_t, uint64_t> m_latestExternalOutputRhiFrameIds;
    };

#if defined(NLS_ENABLE_TEST_HOOKS)
    struct NLS_RENDER_API ThreadedRenderingLifecycleTestAccess final
    {
        static bool TryLockTelemetry(ThreadedRenderingLifecycle& lifecycle);
        static void UnlockTelemetry(ThreadedRenderingLifecycle& lifecycle);
    };
#endif
}
