#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Math/Vector4.h"
#include "Rendering/Context/RenderFramePacket.h"
#include "Rendering/Core/RenderClearValues.h"
#include "Rendering/Data/DrawableObjectDescriptor.h"
#include "Rendering/Data/FrameInfo.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"
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

namespace NLS::Render::UI
{
    struct UiDrawDataSnapshot;
}

namespace NLS::Render::Context
{
    enum class RenderPassCommandKind : uint8_t
    {
        Opaque = 0,
        Transparent,
        Skybox,
        Helper,
        GBuffer,
        Lighting,
        Compute,
        Decal,
        UIOverlay,
        SwapchainOutput,
        ForwardOpaqueDepth,
        ForwardOpaqueColor
    };

    inline constexpr const char kUIOverlayRenderPassDebugName[] = "RHIFrameGraph::UIOverlay";
    inline constexpr const char kSwapchainOutputRenderPassDebugName[] = "RHIFrameGraph::SwapchainOutput";

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
        Data::ObjectDrawConstants objectConstants;
        bool usesObjectIndex = false;
        bool objectBindingImmutable = false;
        std::shared_ptr<RHI::RHIBuffer> indirectArguments;
        uint64_t indirectArgumentOffset = 0;
        // Strong owners include immutable particle frames and their device lifetime.
        std::shared_ptr<const void> resourceOwner;
    };

    struct NLS_RENDER_API RecordedComputeBindingSetInput
    {
        uint32_t setIndex = 0u;
        std::shared_ptr<RHI::RHIBindingSet> bindingSet;
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
        bool useTrackedBeforeState = false;
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
        std::vector<BufferVisibilityTransition> exportedBufferVisibilityTransitions;
        std::vector<BufferResourceAccess> bufferResourceAccesses;
        std::vector<TextureResourceAccess> textureResourceAccesses;
        std::vector<TextureVisibilityTransition> textureVisibilityTransitionsBefore;
        std::vector<TextureVisibilityTransition> exportedTextureVisibilityTransitions;
        std::shared_ptr<RHI::UploadContext> uploadContext;
        std::vector<RHI::UploadTextureRequest> textureUploadsBefore;
        std::shared_ptr<RHI::RHIBuffer> indirectArguments;
        uint64_t indirectArgumentOffset = 0;
        std::shared_ptr<const void> resourceOwner;
        std::vector<TextureVisibilityTransition> textureCopyTransitionsBefore;
        std::vector<RHI::RHITextureCopyDesc> textureCopiesAfter;
        std::shared_ptr<RHI::RHITimestampQuery> timestampBegin, timestampEnd;
    };

    NLS_RENDER_API uint64_t RecordComputeDispatchInputs(RHI::RHICommandBuffer& commandBuffer,
        const std::vector<RecordedComputeDispatchInput>& dispatchInputs, bool recordShaderReadBarriers = true);

    enum class QueueDependencyPolicy : uint8_t
    {
        None = 0,
        Previous,
        LastGraphics,
        LastCompute
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
        std::shared_ptr<const UI::UiDrawDataSnapshot> uiDrawDataSnapshot;
        std::shared_ptr<RHI::RHIGraphicsPipeline> outputPipeline;
        std::shared_ptr<RHI::RHIBindingSet> outputBindingSet;
        std::vector<RecordedDrawCommandInput> recordedDrawCommands;
        // On a graphics pass these execute on its graphics command buffer before BeginRenderPass.
        // Such passes are kept whole; child command recording and draw slicing are disabled.
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
        uint64_t readbackTextureGeneration = 0u;
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
        Decal,
        UIOverlay
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

    struct NLS_RENDER_API LargeSceneCullReasonDebugEntry
    {
        uint64_t sceneId = 0u;
        uint32_t primitiveIndex = ~0u;
        uint32_t primitiveGeneration = 0u;
        uint8_t reason = 0u;
        uint32_t selectedLOD = 0u;
        uint64_t commandOffsetBegin = 0u;
        uint64_t commandOffsetEnd = 0u;
        bool visible = false;
    };

    struct NLS_RENDER_API LargeSceneCullReasonDebugSnapshot
    {
        uint64_t frameSerial = 0u;
        uint64_t sceneId = 0u;
        uint64_t primitiveCount = 0u;
        uint64_t visiblePrimitiveCount = 0u;
        std::array<uint64_t, Data::kLargeSceneCullReasonCount> reasonCounts {};
        std::vector<LargeSceneCullReasonDebugEntry> entries;
    };

    // Produced while resolving the persistent scene on the Render Thread.
    struct NLS_RENDER_API RenderSceneBuildData
    {
        uint64_t sceneGameObjectCount = 0u;
        uint64_t sceneLightCount = 0u;
        uint64_t visibleOpaqueDrawCount = 0u;
        uint64_t visibleDecalDrawCount = 0u;
        uint64_t visibleTransparentDrawCount = 0u;
        uint64_t visibleSkyboxDrawCount = 0u;
        uint64_t visibleHelperDrawCount = 0u;
        LargeSceneCullReasonDebugSnapshot largeSceneCullReasonSnapshot;
        std::vector<uint64_t> streamingDependencyPins;
        std::vector<RecordedDrawCommandInput> recordedDrawCommands;
    };

    struct NLS_RENDER_API PostSubmitBufferReadbackState
    {
        mutable std::mutex mutex;
        std::shared_ptr<RHI::RHICompletionToken> completion;
        RHI::RHIReadbackStatusCode resultCode = RHI::RHIReadbackStatusCode::InvalidArgument;
        std::string resultMessage;
        bool beginAttempted = false;
        bool beginInProgress = false;
        bool beginSucceeded = false;
    };

    struct NLS_RENDER_API PostSubmitBufferReadbackRequest
    {
        // Caller owns source-state correctness and must keep destination storage alive until Poll() completes.
        RHI::RHIBufferReadbackDesc desc;
        std::shared_ptr<PostSubmitBufferReadbackState> state;
        std::shared_ptr<void> destinationKeepAlive;
        bool waitForLastComputeQueueCompletion = false;
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
        uint64_t uiOverlayDrawCount = 0u;
        uint64_t swapchainOutputDrawCount = 0u;
        bool hasOpaquePass = false;
        bool hasDecalPass = false;
        bool hasTransparentPass = false;
        bool hasSkyboxPass = false;
        bool hasHelperPass = false;
        bool hasUIOverlayPass = false;
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
        std::shared_ptr<const UI::UiDrawDataSnapshot> uiDrawDataSnapshot;
        std::vector<uint64_t> uiReferencedTextureIdentities;
        uint64_t uiOverlayPresentationInvalidationGeneration = 0u;
        std::vector<std::shared_ptr<RHI::RHITexture>> extractedTextures;
        std::shared_ptr<RHI::RHITexture> preferredReadbackTexture;
        uint64_t preferredReadbackTextureGeneration = 0u;
        std::vector<PostSubmitBufferReadbackRequest> postSubmitBufferReadbacks;
        std::vector<PostSubmitTextureReadbackRequest> postSubmitTextureReadbacks;
        std::vector<uint64_t> streamingDependencyPins;
        uint64_t externalSceneOutputIdentity = 0u;
        std::vector<uint64_t> externalSceneOutputIdentities;
        uint32_t externalSceneOutputTextureCount = 0u;
        std::shared_ptr<RHI::RHITextureView> externalSceneOutputColorView;
        std::shared_ptr<RHI::RHITextureView> externalSceneOutputDepthView;
    };

    using RenderSceneBuildCommand = std::function<RenderScenePackage(size_t)>;
}
